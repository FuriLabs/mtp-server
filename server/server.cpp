/*
 * Copyright (C) 2013 Canonical Ltd.
 * Copyright (C) 2024 Furi Labs
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Authors: Bardia Mosiri <bardia@furilabs.com>
 */

#include "LinuxMtpDatabase.h"

#include <MtpServer.h>
#include <MtpStorage.h>

#include <chrono>
#include <iostream>
#include <thread>
#include <stdint.h>

#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <pwd.h>

#include <hybris/properties/properties.h>
#include <glog/logging.h>

#include <core/dbus/bus.h>
#include <core/dbus/object.h>
#include <core/dbus/property.h>
#include <core/dbus/service.h>
#include <core/dbus/signal.h>
#include <core/dbus/asio/executor.h>
#include <core/dbus/types/stl/tuple.h>
#include <core/dbus/types/stl/vector.h>
#include <core/dbus/types/struct.h>

using namespace android;
namespace dbus = core::dbus;

namespace core {
dbus::Bus::Ptr the_system_bus() {
    static dbus::Bus::Ptr system_bus = std::make_shared<dbus::Bus>(dbus::WellKnownBus::system);
    return system_bus;
}

struct Login1Session {
    struct Interface {
        inline static std::string& name() {
            static std::string s("org.freedesktop.login1.Session");
            return s;
        }
    };

    struct Properties {
        struct LockedHint {
            inline static std::string name() {
                return "LockedHint";
            };
            typedef Login1Session::Interface Interface;
            typedef bool ValueType;
            static const bool readable = true;
            static const bool writable = false;
        };
    };
};

struct Login1Manager {
    struct Interface {
        inline static std::string& name() {
            static std::string s("org.freedesktop.login1.Manager");
            return s;
        }
    };

    struct ListSessionsEx {
        typedef Login1Manager Interface;

        static const std::string& name() {
            static const std::string s {
                "ListSessionsEx"
            };

            return s;
        }

        inline static const std::chrono::milliseconds default_timeout() { return std::chrono::seconds{1}; }
    };
};

}

namespace core {
namespace dbus {
namespace traits {
template<>
struct Service<core::Login1Session> {
    inline static const std::string& interface_name() {
        static const std::string s {
            "org.freedesktop.login1"
        };
        return s;
    }
};

template<>
struct Service<core::Login1Manager> {
    inline static const std::string& interface_name() {
        static const std::string s {
            "org.freedesktop.login1.Manager"
        };
        return s;
    }
};

}
}
}

namespace {
struct FileSystemConfig {
    static const int file_perm = 0664;
    static const int directory_perm = 0755;
};
}

class MtpDaemon {
private:
    struct passwd *userdata;

    // Mtp stuff
    MtpServer* server;
    MtpStorage* home_storage;
    MtpStorage* sd_card;
    MtpDatabase* mtp_database;

    // Lock management
    dbus::Bus::Ptr bus;
    boost::thread dbus_thread;
    bool screen_locked = true;
    std::shared_ptr<core::dbus::Property<core::Login1Session::Properties::LockedHint>> locked_hint;

    // inotify stuff
    boost::thread notifier_thread;
    boost::thread io_service_thread;

    asio::io_context io_svc;
    asio::executor_work_guard<asio::io_context::executor_type> work;
    asio::posix::stream_descriptor stream_desc;
    asio::streambuf buf;

    int inotify_fd;
    int watch_fd;
    int media_fd;

    // storage
    std::map<std::string, std::tuple<MtpStorage*, bool>> removables;
    bool home_storage_added;

    void add_removable_storage(const char *path, const char *name) {
        static int storageID = MTP_STORAGE_REMOVABLE_RAM;

        /* TODO check removable file system type to set maximum file size */
        MtpStorage *removable = new MtpStorage(
            storageID,
            path,
            name,
            1024 * 1024 * 100,  /* 100 MB reserved space, to avoid filling the disk */
            true,
            UINT64_C(4294967295)  /* 4GB-1, we assume vfat here */);

        storageID++;

        // Only add storage if device is unlocked
        if (!screen_locked) {
            mtp_database->addStoragePath(path,
                                         std::string(),
                                         removable->getStorageID(),
                                         true);
            server->addStorage(removable);
            removables.insert(std::pair<std::string, std::tuple<MtpStorage*, bool>>
                              (name, std::make_tuple(removable, true)));
        } else {
            removables.insert(std::pair<std::string, std::tuple<MtpStorage*, bool>>
                              (name, std::make_tuple(removable, false)));
        }
    }

    void add_mountpoint_watch(const std::string& path) {
        VLOG(1) << "Adding notify watch for " << path;
        watch_fd = inotify_add_watch(inotify_fd,
                                     path.c_str(),
                                     IN_CREATE | IN_DELETE);
    }

    void read_more_notify() {
        VLOG(1) << __PRETTY_FUNCTION__;

        std::size_t available = buf.max_size() - buf.size();

        if (available == 0) {
            LOG(WARNING) << "MtpDaemon inotify buffer full, dropping buffered data";
            buf.consume(buf.size());
            available = buf.max_size();
        }

        stream_desc.async_read_some(buf.prepare(available),
                                    boost::bind(&MtpDaemon::inotify_handler,
                                                this,
                                                asio::placeholders::error,
                                                asio::placeholders::bytes_transferred));
    }

    void inotify_handler(const boost::system::error_code& error,
                         std::size_t transferred) {
        if (error) {
            VLOG(1) << "daemon inotify handler error: " << error.message();
            return;
        }

        buf.commit(transferred);

        size_t processed = 0;

        while (buf.size() - processed >= sizeof(inotify_event)) {
            auto data = buf.data();
            auto begin = asio::buffers_begin(data);

            if (buf.size() - processed < sizeof(inotify_event))
                break;

            const char* cdata = &(*(begin + processed));
            const inotify_event* ievent =
                reinterpret_cast<const inotify_event*>(cdata);

            if (buf.size() - processed < sizeof(inotify_event) + ievent->len)
                break;

            path storage_path ("/media");

            processed += sizeof(inotify_event) + ievent->len;

            storage_path /= userdata->pw_name;

            if (ievent->len > 0 && ievent->mask & IN_CREATE) {
                if (ievent->wd == media_fd) {
                    VLOG(1) << "media root was created for user " << ievent->name;
                    add_mountpoint_watch(storage_path.string());
                } else {
                    VLOG(1) << "Storage was added: " << ievent->name;
                    storage_path /= ievent->name;
                    add_removable_storage(storage_path.string().c_str(), ievent->name);
                }
            } else if (ievent->len > 0 && ievent->mask & IN_DELETE) {
                VLOG(1) << "Storage was removed: " << ievent->name;

                // Try to match to which storage was removed.
                BOOST_FOREACH (std::string name, removables | boost::adaptors::map_keys) {
                    if (name == ievent->name) {
                        auto t = removables.at(name);
                        MtpStorage *storage = std::get<0>(t);
                        bool added = std::get<1>(t);

                        if (added) {
                            VLOG(2) << "removing storage id "
                                    << storage->getStorageID();
                            server->removeStorage(storage);
                            mtp_database->removeStorage(storage->getStorageID());
                        }

                        removables.erase(name);
                        delete storage;
                        break;
                    }
                }
            }
        }

        buf.consume(processed);
        read_more_notify();
    }

    void setup_logind_monitor() {
        try {
            bus = core::the_system_bus();
            bus->install_executor(core::dbus::asio::make_executor(bus));

            auto login1_service = dbus::Service::use_service(bus, "org.freedesktop.login1");
            auto login1_manager = login1_service->object_for_path(dbus::types::ObjectPath("/org/freedesktop/login1"));

            const char* xdg_session_id = nullptr;

            using sessionTypeEx = std::vector<dbus::types::Struct<std::tuple<std::string, uint32_t, std::string, std::string, uint32_t, std::string, std::string, bool, uint64_t, dbus::types::ObjectPath>>>;
            auto sessions = login1_manager->invoke_method_synchronously<core::Login1Manager::ListSessionsEx, sessionTypeEx>();

            struct SessionInfo
            {
                std::string session_id;
                std::string tty;
            };

            for (const auto& session : sessions.value()) {
                SessionInfo sessionInfo;
                std::tie(sessionInfo.session_id, std::ignore, std::ignore, std::ignore, std::ignore, std::ignore, sessionInfo.tty, std::ignore, std::ignore, std::ignore) = session.value;

                if (sessionInfo.tty == "tty7") {
                    xdg_session_id = sessionInfo.session_id.c_str();
                    break;
                }
            }

            if (xdg_session_id != nullptr) {
                std::string session_path = "/org/freedesktop/login1/session/" + std::string(xdg_session_id);
                auto session = login1_service->object_for_path(dbus::types::ObjectPath(session_path));

                // Get the LockedHint property
                locked_hint = session->get_property<core::Login1Session::Properties::LockedHint>();
                screen_locked = locked_hint->get();

                // Monitor for changes
                locked_hint->changed().connect([this](bool locked) {
                    handle_lock_state(locked);
                });
            } else {
                std::cerr << "Error: xdg_session_id is null" << std::endl;
            }
        } catch (const std::exception& e) {
            LOG(ERROR) << "Failed to setup logind monitor: " << e.what();
        }
    }
    void handle_lock_state(bool locked) {
        screen_locked = locked;
        if (!locked) {
            VLOG(2) << "Screen unlocked, adding storage";
            if (home_storage && !home_storage_added) {
                server->addStorage(home_storage);
                home_storage_added = true;
            }

            BOOST_FOREACH(std::string name, removables | boost::adaptors::map_keys) {
                auto t = removables.at(name);
                MtpStorage *storage = std::get<0>(t);
                bool added = std::get<1>(t);
                if (!added) {
                    mtp_database->addStoragePath(storage->getPath(),
                                               std::string(),
                                               storage->getStorageID(),
                                               true);
                    server->addStorage(storage);
                    std::get<1>(removables[name]) = true;
                }
            }
        } else {
            VLOG(2) << "Screen locked, removing storage";
            if (home_storage && home_storage_added) {
                server->removeStorage(home_storage);
                home_storage_added = false;
            }

            BOOST_FOREACH(std::string name, removables | boost::adaptors::map_keys) {
                auto t = removables.at(name);
                MtpStorage *storage = std::get<0>(t);
                bool added = std::get<1>(t);
                if (added) {
                    server->removeStorage(storage);
                    mtp_database->removeStorage(storage->getStorageID());
                    std::get<1>(removables[name]) = false;
                }
            }
        }
    }

    void drive_bus() {
        try {
            bus->run();
        } catch (const std::exception& e) {
            LOG(ERROR) << "DBus error: " << e.what();
        }
    }

public:
    MtpDaemon(int fd):
        server(nullptr),
        home_storage(nullptr),
        sd_card(nullptr),
        mtp_database(nullptr),
        work(asio::make_work_guard(io_svc)),
        stream_desc(io_svc),
        buf(1024),
        inotify_fd(-1),
        watch_fd(-1),
        media_fd(-1),
        home_storage_added(false) {
        userdata = getpwuid(getuid());

        // Removable storage hacks
        inotify_fd = inotify_init();
        if (inotify_fd <= 0)
            PLOG(FATAL) << "Unable to initialize inotify";
        VLOG(1) << "using inotify fd " << inotify_fd << " for daemon";

        stream_desc.assign(inotify_fd);
        notifier_thread = boost::thread(&MtpDaemon::read_more_notify, this);
        io_service_thread = boost::thread([this]() { io_svc.run(); });

        // MTP database.
        mtp_database = new LinuxMtpDatabase();

        // MTP server
        server = new MtpServer(
                fd,
                mtp_database,
                false,
                userdata->pw_gid,
                FileSystemConfig::file_perm,
                FileSystemConfig::directory_perm);

        // Setup logind monitoring
        setup_logind_monitor();
        dbus_thread = boost::thread(&MtpDaemon::drive_bus, this);
    }

    void initStorage() {
        char product_name[PROP_VALUE_MAX];

        // Local storage
        property_get("ro.product.model", product_name, "FuriOS Device");

        std::string current_directory = userdata->pw_dir;

        home_storage = new MtpStorage(
            MTP_STORAGE_FIXED_RAM,
            userdata->pw_dir,
            product_name,
            1024 * 1024 * 100,  /* 100 MB reserved space, to avoid filling the disk */
            false,
            0  /* Do not check sizes for internal storage */);

        mtp_database->addStoragePath(current_directory, "", MTP_STORAGE_FIXED_RAM, false);
        home_storage_added = false;

        // Get any already-mounted removable storage.
        path p(std::string("/media/") + userdata->pw_name);
        if (exists(p)) {
            std::vector<path> v;
            copy(directory_iterator(p), directory_iterator(), std::back_inserter(v));
            for (std::vector<path>::const_iterator it(v.begin()), it_end(v.end()); it != it_end; ++it) {
                add_removable_storage(it->string().c_str(), it->filename().c_str());
            }

            // make sure we can catch any new removable storage that gets added.
            add_mountpoint_watch(p.string());
        } else {
            media_fd = inotify_add_watch(inotify_fd,
                                         "/media",
                                         IN_CREATE | IN_DELETE);
        }
    }

    ~MtpDaemon() {
        // Cleanup
        if (watch_fd >= 0)
            inotify_rm_watch(inotify_fd, watch_fd);
        if (media_fd >= 0)
            inotify_rm_watch(inotify_fd, media_fd);

        work.reset();
        io_svc.stop();
        if (notifier_thread.joinable())
            notifier_thread.join();
        if (io_service_thread.joinable())
            io_service_thread.join();
        if (inotify_fd >= 0)
            close(inotify_fd);

        dbus_thread.interrupt();
        if (dbus_thread.joinable())
            dbus_thread.join();

        delete server;
        delete home_storage;
        delete mtp_database;

        BOOST_FOREACH(std::string name, removables | boost::adaptors::map_keys) {
            auto t = removables.at(name);
            MtpStorage *storage = std::get<0>(t);
            delete storage;
        }
    }

    void run() {
        // Initial storage state based on lock status
        handle_lock_state(screen_locked);

        // start the MtpServer main loop
        server->run();
    }
};

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);

    LOG(INFO) << "MTP server starting...";

    int fd = -1;

    for (int i = 0; i < 15; ++i) {
        fd = open("/dev/mtp_usb", O_RDWR);

        if (fd >= 0)
            break;

        LOG(INFO) << "Couldn't open /dev/mtp_usb, attempt "
                  << (i + 1) << "/15, waiting 1 second...";

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (fd < 0) {
        LOG(ERROR) << "Failed to open /dev/mtp_usb after 15 attempts";
        return 0;
    }

    try {
        MtpDaemon *d = new MtpDaemon(fd);

        d->initStorage();
        d->run();

        delete d;
    }
    catch (std::exception& e) {
        /* If the daemon fails to initialize, ignore the error but
         * make sure to propagate the message and return with an
         * error return code.
         */
        LOG(ERROR) << "Could not start the MTP server: " << e.what();
    }

    return 0;
}
