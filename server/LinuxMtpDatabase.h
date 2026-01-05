/*
 * Copyright (C) 2013 Canonical Ltd.
 * Copyright (C) 2025 Furi Labs.
 *
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
 */

#ifndef STUB_MTP_DATABASE_H_
#define STUB_MTP_DATABASE_H_

#include <mtp.h>
#include <MtpDatabase.h>
#include <MtpDataPacket.h>
#include <MtpStringBuffer.h>
#include <MtpObjectInfo.h>
#include <MtpProperty.h>
#include <MtpDebug.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>
#include <string>
#include <tuple>
#include <exception>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <cstdint>

#include <boost/thread.hpp>
#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/foreach.hpp>
#include <boost/filesystem.hpp>
#include <boost/range/adaptors.hpp>
#include <boost/range/algorithm.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <glog/logging.h>

#define ALL_PROPERTIES 0xffffffff

namespace asio = boost::asio;
using namespace boost::filesystem;

namespace android
{
class LinuxMtpDatabase : public android::MtpDatabase {
private:
    struct DbEntry
    {
        MtpStorageID storage_id;
        MtpObjectFormat object_format;
        MtpObjectHandle parent;
        uint64_t object_size;
        std::string display_name;
        std::string path;
        int watch_fd;
        std::time_t last_modified;
    };

    MtpServer* local_server;
    uint32_t counter;
    std::map<MtpObjectHandle, DbEntry> db;
    std::map<std::string, MtpObjectFormat> formats = boost::assign::map_list_of
        (".gif", MTP_FORMAT_GIF)
        (".png", MTP_FORMAT_PNG)
        (".jpeg", MTP_FORMAT_JFIF)
        (".tiff", MTP_FORMAT_TIFF)
        (".ogg", MTP_FORMAT_OGG)
        (".mp3", MTP_FORMAT_MP3)
        (".wav", MTP_FORMAT_WAV)
        (".wma", MTP_FORMAT_WMA)
        (".aac", MTP_FORMAT_AAC)
        (".flac", MTP_FORMAT_FLAC);

    boost::thread notifier_thread;
    boost::thread io_service_thread;

    asio::io_service io_svc;
    asio::io_service::work work;
    asio::posix::stream_descriptor stream_desc;
    asio::streambuf buf;
    int inotify_fd;

    struct DevIno {
        std::uint64_t dev;
        std::uint64_t ino;
        bool operator==(const DevIno& o) const noexcept { return dev == o.dev && ino == o.ino; }
    };

    struct DevInoHash {
        std::size_t operator()(const DevIno& d) const noexcept {
            return std::hash<std::uint64_t>()(d.dev) ^ (std::hash<std::uint64_t>()(d.ino) << 1);
        }
    };

    static bool stat_dev_ino(const std::string& p, DevIno& out) {
        struct stat st;
        if (::lstat(p.c_str(), &st) != 0)
            return false;
        out.dev = static_cast<std::uint64_t>(st.st_dev);
        out.ino = static_cast<std::uint64_t>(st.st_ino);
        return true;
    }

    MtpObjectFormat guess_object_format(std::string extension)
    {
        std::map<std::string, MtpObjectFormat>::iterator it;

        it = formats.find(extension);
        if (it == formats.end()) {
            boost::to_upper(extension);
            it = formats.find(extension);
            if (it == formats.end()) {
                return MTP_FORMAT_UNDEFINED;
            }
        }

        return it->second;
    }

    int setup_dir_inotify(path p)
    {
        int wd = inotify_add_watch(inotify_fd,
                                   p.string().c_str(),
                                   IN_MODIFY | IN_CREATE | IN_DELETE);
        // not fatal, indexing must continue even if we can't watch.
        if (wd < 0)
            PLOG(INFO) << "inotify_add_watch failed for " << p.string();
        return wd;
    }

    static bool is_directory_nofollow(const path& p, boost::system::error_code& ec_out)
    {
        ec_out.clear();
        file_status st = symlink_status(p, ec_out);
        if (ec_out)
            return false;
        // do not follow symlink dirs
        if (is_symlink(st))
            return false;
        return is_directory(st);
    }

    static bool is_regular_file_nofollow(const path& p, boost::system::error_code& ec_out)
    {
        ec_out.clear();
        file_status st = symlink_status(p, ec_out);
        if (ec_out)
            return false;
        // skip symlinks entirely
        if (is_symlink(st))
            return false;
        return is_regular_file(st);
    }

    MtpObjectHandle add_file_entry(path p, MtpObjectHandle parent, MtpStorageID storage)
    {
        // never let exceptions kill indexing
        try {
            boost::system::error_code ec;

            // prevents loops
            file_status st = symlink_status(p, ec);
            if (ec) {
                VLOG(2) << "Skipping (stat failed): " << p.string() << " : " << ec.message();
                return 0;
            }
            if (is_symlink(st)) {
                VLOG(2) << "Skipping symlink: " << p.string();
                return 0;
            }

            if (!is_directory(st) && !is_regular_file(st)) {
                // skip special files (fifo, socket, device nodes, etc.)
                VLOG(2) << "Skipping non-regular: " << p.string();
                return 0;
            }

            MtpObjectHandle handle = counter;
            DbEntry entry;
            counter++;

            entry.storage_id = storage;
            entry.parent = parent;
            entry.display_name = std::string(p.filename().string());
            entry.path = p.string();

            if (is_directory(st)) {
                entry.object_format = MTP_FORMAT_ASSOCIATION;
                entry.object_size = 0;
                entry.watch_fd = setup_dir_inotify(p);

                // last_write_time can throw
                std::time_t lwt = 0;
                ec.clear();
                lwt = last_write_time(p, ec);
                entry.last_modified = ec ? 0 : lwt;

                db.insert(std::pair<MtpObjectHandle, DbEntry>(handle, entry));

                if (local_server)
                    local_server->sendObjectAdded(handle);

                return handle;
            } else {
                entry.object_format = guess_object_format(p.extension().string());

                // file_size / last_write_time can error
                ec.clear();
                uintmax_t sz = file_size(p, ec);
                entry.object_size = ec ? 0 : static_cast<uint64_t>(sz);

                ec.clear();
                std::time_t lwt = last_write_time(p, ec);
                entry.last_modified = ec ? 0 : lwt;

                entry.watch_fd = -1;

                VLOG(1) << "Adding \"" << p.string() << "\"";
                db.insert(std::pair<MtpObjectHandle, DbEntry>(handle, entry));

                if (local_server)
                    local_server->sendObjectAdded(handle);

                return handle;
            }
        } catch (const std::exception& e) {
            LOG(WARNING) << "add_file_entry suppressed exception for " << p.string() << " : " << e.what();
            return 0;
        } catch (...) {
            LOG(WARNING) << "add_file_entry suppressed unknown exception for " << p.string();
            return 0;
        }
    }

    void parse_directory(path p, MtpObjectHandle parent, MtpStorageID storage)
    {
        try {
            // limits so a pathological tree can’t wedge forever
            const int max_depth = 64;
            const std::uint64_t max_items = 2000000ULL;

            std::uint64_t items_seen = 0;

            struct StackItem {
                path dir;
                MtpObjectHandle handle;
                int depth;
            };

            std::unordered_set<DevIno, DevInoHash> visited;
            std::vector<StackItem> stack;

            {
                boost::system::error_code ec;
                if (!is_directory_nofollow(p, ec)) {
                    if (ec)
                        VLOG(2) << "parse_directory: not accessible: " << p.string() << " : " << ec.message();
                    return;
                }

                DevIno di;
                if (stat_dev_ino(p.string(), di))
                    visited.insert(di);
                stack.push_back(StackItem{p, parent, 0});
            }

            while (!stack.empty()) {
                if (items_seen >= max_items) {
                    LOG(WARNING) << "Index: max_items reached under " << p.string() << ", stopping traversal";
                    return;
                }

                StackItem cur = stack.back();
                stack.pop_back();

                if (cur.depth > max_depth) {
                    VLOG(1) << "Index: max_depth reached at " << cur.dir.string() << ", skipping";
                    continue;
                }

                boost::system::error_code ec;
                directory_iterator it(cur.dir, ec);
                if (ec) {
                    VLOG(2) << "Index: cannot open dir " << cur.dir.string() << " : " << ec.message();
                    continue;
                }

                directory_iterator end;
                for (; it != end; ) {
                    path entry_path;

                    try {
                        entry_path = it->path();
                    } catch (...) {
                        ec.clear();
                        it.increment(ec);
                        if (ec)
                            break;
                        continue;
                    }

                    ec.clear();
                    it.increment(ec);

                    boost::system::error_code ec2;
                    file_status st = symlink_status(entry_path, ec2);
                    if (ec2) {
                        VLOG(2) << "Index: cannot stat " << entry_path.string() << " : " << ec2.message();
                        if (ec)
                            break;
                        continue;
                    }
                    if (is_symlink(st)) {
                        VLOG(2) << "Index: skipping symlink " << entry_path.string();
                        if (ec)
                            break;
                        continue;
                    }

                    if (is_directory(st)) {
                        MtpObjectHandle dir_handle = add_file_entry(entry_path, cur.handle, storage);
                        if (dir_handle != 0) {
                            DevIno di;
                            if (stat_dev_ino(entry_path.string(), di)) {
                                if (visited.find(di) == visited.end()) {
                                    visited.insert(di);
                                    stack.push_back(StackItem{entry_path, dir_handle, cur.depth + 1});
                                } else {
                                    VLOG(2) << "Index: skipping already visited dir " << entry_path.string();
                                }
                            } else {
                                stack.push_back(StackItem{entry_path, dir_handle, cur.depth + 1});
                            }
                        }
                    } else {
                        add_file_entry(entry_path, cur.handle, storage);
                    }

                    items_seen++;

                    if (ec) {
                        VLOG(2) << "Index: iterator error in " << cur.dir.string() << " : " << ec.message();
                        break;
                    }
                }
            }
        } catch (const std::exception& e) {
            LOG(WARNING) << "parse_directory suppressed exception for " << p.string() << " : " << e.what();
        } catch (...) {
            LOG(WARNING) << "parse_directory suppressed unknown exception for " << p.string();
        }
    }

    void readFiles(const std::string& sourcedir, const std::string& display, MtpStorageID storage, bool hidden)
    {
        path p(sourcedir);
        DbEntry entry;
        MtpObjectHandle handle = counter++;
        std::string display_name = std::string(p.filename().string());

        if (!display.empty())
            display_name = display;

        try {
            boost::system::error_code ec;

            if (exists(p, ec) && !ec) {
                file_status st = symlink_status(p, ec);
                if (ec) {
                    LOG(WARNING) << "Cannot stat " << p.string() << " : " << ec.message();
                    return;
                }
                if (is_symlink(st)) {
                    LOG(INFO) << "Skipping symlink root " << p.string();
                    return;
                }

                if (is_directory(st)) {
                    entry.storage_id = storage;
                    entry.parent = hidden ? MTP_PARENT_ROOT : 0;
                    entry.display_name = display_name;
                    entry.path = p.string();
                    entry.object_format = MTP_FORMAT_ASSOCIATION;
                    entry.object_size = 0;
                    entry.watch_fd = setup_dir_inotify(p);

                    ec.clear();
                    std::time_t lwt = last_write_time(p, ec);
                    entry.last_modified = ec ? 0 : lwt;

                    db.insert(std::pair<MtpObjectHandle, DbEntry>(handle, entry));
                    parse_directory(p, hidden ? 0 : handle, storage);
                } else {
                    LOG(WARNING) << p << " is not a directory.";
                }
            } else {
                if (storage == MTP_STORAGE_FIXED_RAM) {
                    LOG(WARNING) << p << " does not exist.";
                } else {
                    entry.storage_id = storage;
                    entry.parent = -1;
                    entry.display_name = display_name;
                    entry.path = p.parent_path().string();
                    entry.object_format = MTP_FORMAT_ASSOCIATION;
                    entry.object_size = 0;
                    entry.watch_fd = setup_dir_inotify(p.parent_path());
                    entry.last_modified = 0;
                }
            }
        } catch (const filesystem_error& ex) {
            LOG(ERROR) << ex.what();
        } catch (const std::exception& e) {
            LOG(ERROR) << e.what();
        } catch (...) {
            LOG(ERROR) << "readFiles: unexpected error";
        }
    }

    void read_more_notify()
    {
        stream_desc.async_read_some(buf.prepare(buf.max_size()),
                                    boost::bind(&LinuxMtpDatabase::inotify_handler,
                                                this,
                                                asio::placeholders::error,
                                                asio::placeholders::bytes_transferred));
    }

    void inotify_handler(const boost::system::error_code&,
                         std::size_t transferred)
    {
        size_t processed = 0;

        while (transferred - processed >= sizeof(inotify_event))
        {
            const char* cdata = processed + asio::buffer_cast<const char*>(buf.data());
            const inotify_event* ievent = reinterpret_cast<const inotify_event*>(cdata);
            MtpObjectHandle parent = 0;
            path p;

            processed += sizeof(inotify_event) + ievent->len;

            BOOST_FOREACH(MtpObjectHandle i, db | boost::adaptors::map_keys) {
                if (db.at(i).watch_fd == ievent->wd) {
                    parent = i;
                    break;
                }
            }

            try {
                p = path(db.at(parent).path + "/" + ievent->name);
            } catch (...) {
                PLOG(WARNING) << "Could not find parent for event " << ievent->name;
                continue;
            }

            if (ievent->len > 0 && (ievent->mask & IN_MODIFY))
            {
                VLOG(2) << __PRETTY_FUNCTION__ << ": file modified: " << p.string();
                BOOST_FOREACH(MtpObjectHandle i, db | boost::adaptors::map_keys) {
                    if (db.at(i).path == p.string()) {
                        boost::system::error_code ec;
                        uintmax_t sz = file_size(p, ec);
                        if (!ec) {
                            VLOG(2) << "new size: " << sz;
                            db.at(i).object_size = static_cast<uint64_t>(sz);
                        } else {
                            PLOG(INFO) << "Could not read file size for " << p.string();
                        }
                    }
                }
            }
            else if (ievent->len > 0 && (ievent->mask & IN_CREATE))
            {
                int parent_handle = parent;
                bool exists_in_db = false;

                VLOG(2) << __PRETTY_FUNCTION__ << ": file created: " << p.string();
                BOOST_FOREACH(MtpObjectHandle i, db | boost::adaptors::map_keys) {
                    if (db.at(i).path == p.string()) {
                        exists_in_db = true;
                        break;
                    }
                }

                if (!exists_in_db) {
                    /* Deal with the special case where the SD card might initially
                     * require an inotify watch, because it's not yet mounted.
                     * In this case, the SD card inotify watch is entered as a
                     * normal object, but the parent for the "real" directory
                     * for the mounted removable media should be the MTP root
                     * for the storage ID.
                     */
                    if (db.at(parent).parent == MTP_PARENT_ROOT)
                        parent_handle = 0;

                    /* try to deal with it as if it was a file. */
                    MtpObjectHandle h = add_file_entry(p, parent_handle, db.at(parent).storage_id);
                    if (h != 0) {
                        boost::system::error_code ec3;
                        file_status st3 = symlink_status(p, ec3);
                        if (!ec3 && is_directory(st3))
                            parse_directory(p, h, db.at(parent).storage_id);
                    }
                }
            }
            else if (ievent->len > 0 && (ievent->mask & IN_DELETE))
            {
                VLOG(2) << __PRETTY_FUNCTION__ << ": file deleted: " << p.string();
                BOOST_FOREACH(MtpObjectHandle i, db | boost::adaptors::map_keys) {
                    if (db.at(i).path == p.string()) {
                        VLOG(2) << "deleting file at handle " << i;
                        deleteFile(i);
                        if (local_server)
                            local_server->sendObjectRemoved(i);
                        break;
                    }
                }
            }
        }

        read_more_notify();
    }

public:
    LinuxMtpDatabase():
        counter(1),
        stream_desc(io_svc),
        work(io_svc),
        buf(1024)
    {
        local_server = nullptr;

        inotify_fd = inotify_init();
        if (inotify_fd <= 0)
            PLOG(FATAL) << "Invalid file descriptor to inotify";
        VLOG(1) << "using inotify fd " << inotify_fd << " for database";

        stream_desc.assign(inotify_fd);

        db = std::map<MtpObjectHandle, DbEntry>();

        notifier_thread = boost::thread(&LinuxMtpDatabase::read_more_notify, this);
        io_service_thread = boost::thread(boost::bind(&asio::io_service::run, &io_svc));
    }

    virtual ~LinuxMtpDatabase() {
        io_svc.stop();
        notifier_thread.detach();
        io_service_thread.join();
        close(inotify_fd);
    }

    virtual void addStoragePath(const MtpString& path,
                                const MtpString& displayName,
                                MtpStorageID storage,
                                bool hidden)
    {
        readFiles(path, displayName, storage, hidden);
    }

    virtual void removeStorage(MtpStorageID storage)
    {
        // remove all database entries corresponding to said storage.
        for (std::map<MtpObjectHandle, DbEntry>::iterator it = db.begin(); it != db.end(); ) {
            if (it->second.storage_id == storage)
                it = db.erase(it);
            else
                ++it;
        }
    }

    // called from SendObjectInfo to reserve a database entry for the incoming file
    virtual MtpObjectHandle beginSendObject(
        const MtpString& path,
        MtpObjectFormat format,
        MtpObjectHandle parent,
        MtpStorageID storage,
        uint64_t size,
        time_t modified)
    {
        DbEntry entry;
        MtpObjectHandle handle = counter;

        if (storage == MTP_STORAGE_FIXED_RAM && parent == 0)
            return kInvalidObjectHandle;

        VLOG(1) << __PRETTY_FUNCTION__ << ": " << path << " - " << parent
                << " format: " << std::hex << format << std::dec;

        entry.storage_id = storage;
        entry.parent = parent;
        entry.display_name = std::string(basename(path.c_str()));
        entry.path = path;
        entry.object_format = format;
        entry.object_size = size;
        entry.last_modified = modified;

        db.insert(std::pair<MtpObjectHandle, DbEntry>(handle, entry));

        counter++;

        return handle;
    }

    // called to report success or failure of the SendObject file transfer
    // success should signal a notification of the new object's creation,
    // failure should remove the database entry created in beginSendObject
    virtual void endSendObject(
        const MtpString& path,
        MtpObjectHandle handle,
        MtpObjectFormat format,
        bool succeeded)
    {
        VLOG(1) << __PRETTY_FUNCTION__ << ": " << path;

        try
        {
            if (!succeeded) {
                db.erase(handle);
            } else {
                boost::filesystem::path p(path);

                if (format != MTP_FORMAT_ASSOCIATION) {
                    /* Resync file size, just in case this is actually an Edit. */
                    boost::system::error_code ec;
                    uintmax_t sz = file_size(p, ec);
                    if (!ec)
                        db.at(handle).object_size = static_cast<uint64_t>(sz);
                }
            }
        } catch(...)
        {
            LOG(ERROR) << __PRETTY_FUNCTION__
                       << ": failed to complete object creation:" << path;
        }
    }

    virtual MtpObjectHandleList* getObjectList(
        MtpStorageID storageID,
        MtpObjectFormat format,
        MtpObjectHandle parent)
    {
        VLOG(1) << __PRETTY_FUNCTION__ << ": " << storageID << ", " << format << ", " << parent;
        MtpObjectHandleList* list = nullptr;

        if (parent == MTP_PARENT_ROOT)
            parent = 0;

        try
        {
            std::vector<MtpObjectHandle> keys;

            BOOST_FOREACH(MtpObjectHandle i, db | boost::adaptors::map_keys) {
                if (db.at(i).storage_id == storageID && db.at(i).parent == parent)
                    if (format == 0 || db.at(i).object_format == format)
                        keys.push_back(i);
            }

            list = new MtpObjectHandleList(keys);
        } catch(...)
        {
            list = new MtpObjectHandleList();
        }

        return list;
    }

    virtual int getNumObjects(
        MtpStorageID storageID,
        MtpObjectFormat format,
        MtpObjectHandle parent)
    {
        VLOG(1) << __PRETTY_FUNCTION__ << ": " << storageID << ", " << format << ", " << parent;

        int result = 0;

        try
        {
            MtpObjectHandleList *list = getObjectList(storageID, format, parent);
            result = list->size();
            delete list;
        } catch(...)
        {
        }

        return result;
    }

    // callee should delete[] the results from these
    // results can be NULL
    virtual MtpObjectFormatList* getSupportedPlaybackFormats()
    {
        VLOG(1) << __PRETTY_FUNCTION__;
        static const MtpObjectFormatList list = {
            /* Generic files */
            MTP_FORMAT_UNDEFINED,
            MTP_FORMAT_ASSOCIATION, // folders
            MTP_FORMAT_TEXT,
            MTP_FORMAT_HTML,

            /* Supported image formats */
            MTP_FORMAT_DEFINED, // generic image
            MTP_FORMAT_EXIF_JPEG,
            MTP_FORMAT_TIFF_EP,
            MTP_FORMAT_BMP,
            MTP_FORMAT_GIF,
            MTP_FORMAT_JFIF,
            MTP_FORMAT_PNG,
            MTP_FORMAT_TIFF,
            MTP_FORMAT_TIFF_IT,
            MTP_FORMAT_JP2,
            MTP_FORMAT_JPX,

            /* Supported audio formats */
            MTP_FORMAT_OGG,
            MTP_FORMAT_MP3,
            MTP_FORMAT_WAV,
            MTP_FORMAT_WMA,
            MTP_FORMAT_AAC,
            MTP_FORMAT_FLAC,

            /* Supported video formats */
            // none listed yet, video apparently broken.

            /* Audio album, and album art */
            MTP_FORMAT_ABSTRACT_AUDIO_ALBUM,

            /* Playlists for audio and video */
            MTP_FORMAT_ABSTRACT_AV_PLAYLIST,
        };

        return new MtpObjectFormatList{list};
    }

    virtual MtpObjectFormatList* getSupportedCaptureFormats()
    {
        VLOG(1) << __PRETTY_FUNCTION__;
        static const MtpObjectFormatList list = {MTP_FORMAT_ASSOCIATION, MTP_FORMAT_PNG};
        return new MtpObjectFormatList{list};
    }

    virtual MtpObjectPropertyList* getSupportedObjectProperties(MtpObjectFormat format)
    {
        VLOG(1) << __PRETTY_FUNCTION__;
        static const MtpObjectPropertyList list =
        {
            MTP_PROPERTY_STORAGE_ID,
            MTP_PROPERTY_PARENT_OBJECT,
            MTP_PROPERTY_OBJECT_FORMAT,
            MTP_PROPERTY_OBJECT_SIZE,
            MTP_PROPERTY_OBJECT_FILE_NAME,
            MTP_PROPERTY_DISPLAY_NAME,
            MTP_PROPERTY_PERSISTENT_UID,
            MTP_PROPERTY_ASSOCIATION_TYPE,
            MTP_PROPERTY_ASSOCIATION_DESC,
            MTP_PROPERTY_PROTECTION_STATUS,
            MTP_PROPERTY_DATE_CREATED,
            MTP_PROPERTY_DATE_MODIFIED,
            MTP_PROPERTY_HIDDEN,
            MTP_PROPERTY_NON_CONSUMABLE,
        };

        return new MtpObjectPropertyList{list};
    }

    virtual MtpDevicePropertyList* getSupportedDeviceProperties()
    {
        VLOG(1) << __PRETTY_FUNCTION__;
        static const MtpDevicePropertyList list = {
            MTP_DEVICE_PROPERTY_DEVICE_FRIENDLY_NAME,
            MTP_DEVICE_PROPERTY_SYNCHRONIZATION_PARTNER,
        };
        return new MtpDevicePropertyList{list};
    }

    virtual MtpResponseCode getObjectPropertyValue(
        MtpObjectHandle handle,
        MtpObjectProperty property,
        MtpDataPacket& packet)
    {
        char date[20];

        VLOG(1) << __PRETTY_FUNCTION__
                << " handle: " << handle
                << " property: " << MtpDebug::getObjectPropCodeName(property);

        if (handle == MTP_PARENT_ROOT || handle == 0)
            return MTP_RESPONSE_INVALID_OBJECT_HANDLE;

        try {
            switch(property)
            {
                case MTP_PROPERTY_STORAGE_ID: packet.putUInt32(db.at(handle).storage_id); break;
                case MTP_PROPERTY_PARENT_OBJECT: packet.putUInt32(db.at(handle).parent); break;
                case MTP_PROPERTY_OBJECT_FORMAT: packet.putUInt16(db.at(handle).object_format); break;
                case MTP_PROPERTY_OBJECT_SIZE: packet.putUInt64(db.at(handle).object_size); break;
                case MTP_PROPERTY_DISPLAY_NAME: packet.putString(db.at(handle).display_name.c_str()); break;
                case MTP_PROPERTY_OBJECT_FILE_NAME: packet.putString(db.at(handle).display_name.c_str()); break;
                case MTP_PROPERTY_PERSISTENT_UID: packet.putUInt128(handle); break;
                case MTP_PROPERTY_ASSOCIATION_TYPE:
                    if (db.at(handle).object_format == MTP_FORMAT_ASSOCIATION)
                        packet.putUInt16(MTP_ASSOCIATION_TYPE_GENERIC_FOLDER);
                    else
                        packet.putUInt16(0);
                    break;
                case MTP_PROPERTY_ASSOCIATION_DESC: packet.putUInt32(0); break;
                case MTP_PROPERTY_PROTECTION_STATUS:
                    packet.putUInt16(0x0000); // no files are read-only for now.
                    break;
                case MTP_PROPERTY_DATE_CREATED:
                    formatDateTime(0, date, sizeof(date));
                    packet.putString(date);
                    break;
                case MTP_PROPERTY_DATE_MODIFIED:
                    formatDateTime(db.at(handle).last_modified, date, sizeof(date));
                    packet.putString(date);
                    break;
                case MTP_PROPERTY_HIDDEN: packet.putUInt16(0); break;
                case MTP_PROPERTY_NON_CONSUMABLE: break;
                    if (db.at(handle).object_format == MTP_FORMAT_ASSOCIATION)
                        packet.putUInt16(0); // folders are non-consumable
                    else
                        packet.putUInt16(1); // files can usually be played.
                    break;
                default: return MTP_RESPONSE_GENERAL_ERROR; break;
            }

            return MTP_RESPONSE_OK;
        }
        catch (...) {
            LOG(ERROR) << __PRETTY_FUNCTION__
                       << "Could not retrieve property: "
                       << MtpDebug::getObjectPropCodeName(property)
                       << " for handle: " << handle;
            return MTP_RESPONSE_GENERAL_ERROR;
        }
    }

    virtual MtpResponseCode setObjectPropertyValue(
        MtpObjectHandle handle,
        MtpObjectProperty property,
        MtpDataPacket& packet)
    {
        DbEntry entry;
        MtpStringBuffer buffer;
        std::string newname;
        path oldpath;
        path newpath;

        VLOG(1) << __PRETTY_FUNCTION__
                << " handle: " << handle
                << " property: " << MtpDebug::getObjectPropCodeName(property);

        if (handle == MTP_PARENT_ROOT || handle == 0)
            return MTP_RESPONSE_INVALID_OBJECT_HANDLE;

        switch(property)
        {
            case MTP_PROPERTY_OBJECT_FILE_NAME:
                try {
                    entry = db.at(handle);

                    if (!packet.getString(buffer)) {
                        LOG(ERROR) << "Cannot read packet";
                        return MTP_RESPONSE_GENERAL_ERROR;
                    }
                    newname = strdup(buffer);

                    oldpath /= entry.path;
                    newpath /= oldpath.parent_path() / "/" / newname;

                    boost::filesystem::rename(oldpath, newpath);

                    db.at(handle).display_name = newname;
                    db.at(handle).path = newpath.string();
                } catch (filesystem_error& fe) {
                    LOG(ERROR) << fe.what();
                    return MTP_RESPONSE_DEVICE_BUSY;
                } catch (std::exception& e) {
                    LOG(ERROR) << e.what();
                    return MTP_RESPONSE_GENERAL_ERROR;
                } catch (...) {
                    LOG(ERROR) << "An unexpected error has occurred";
                    return MTP_RESPONSE_GENERAL_ERROR;
                }

                break;
            case MTP_PROPERTY_PARENT_OBJECT:
                try {
                    entry = db.at(handle);
                    if (!packet.getUInt32(entry.parent)) {
                        LOG(ERROR) << "Cannot read packet";
                        return MTP_RESPONSE_GENERAL_ERROR;
                    }
                }
                catch (...) {
                    LOG(ERROR) << "Could not change parent object for handle "
                               << handle;
                    return MTP_RESPONSE_GENERAL_ERROR;
                }
            default: return MTP_RESPONSE_OPERATION_NOT_SUPPORTED; break;
        }

        return MTP_RESPONSE_OK;
    }

    virtual MtpResponseCode getDevicePropertyValue(
        MtpDeviceProperty property,
        MtpDataPacket& packet)
    {
        VLOG(1) << __PRETTY_FUNCTION__;
        switch(property)
        {
            case MTP_DEVICE_PROPERTY_SYNCHRONIZATION_PARTNER:
            case MTP_DEVICE_PROPERTY_DEVICE_FRIENDLY_NAME:
                packet.putString("");
                break;
            default: return MTP_RESPONSE_OPERATION_NOT_SUPPORTED; break;
        }

        return MTP_RESPONSE_OK;
    }

    virtual MtpResponseCode setDevicePropertyValue(
        MtpDeviceProperty property,
        MtpDataPacket& packet)
    {
        VLOG(1) << __PRETTY_FUNCTION__;
        return MTP_RESPONSE_DEVICE_PROP_NOT_SUPPORTED;
    }

    virtual MtpResponseCode resetDeviceProperty(
        MtpDeviceProperty property)
    {
        VLOG(1) << __PRETTY_FUNCTION__;
        return MTP_RESPONSE_DEVICE_PROP_NOT_SUPPORTED;
    }

    virtual MtpResponseCode getObjectPropertyList(
        MtpObjectHandle handle,
        uint32_t format,
        uint32_t property,
        int groupCode,
        int depth,
        MtpDataPacket& packet)
    {
        std::vector<MtpObjectHandle> handles;

        VLOG(2) << __PRETTY_FUNCTION__;

        if (handle == kInvalidObjectHandle)
            return MTP_RESPONSE_PARAMETER_NOT_SUPPORTED;

        if (property == 0 && groupCode == 0)
            return MTP_RESPONSE_PARAMETER_NOT_SUPPORTED;

        if (groupCode != 0)
            return MTP_RESPONSE_SPECIFICATION_BY_GROUP_UNSUPPORTED;

        if (depth > 1)
            return MTP_RESPONSE_SPECIFICATION_BY_DEPTH_UNSUPPORTED;

        if (depth == 0) {
            /* For a depth search, a handle of 0 is valid (objects at the root)
             * but it isn't when querying for the properties of a single object.
             */
            if (db.find(handle) == db.end())
                return MTP_RESPONSE_INVALID_OBJECT_HANDLE;

            handles.push_back(handle);
        } else {
            BOOST_FOREACH(MtpObjectHandle i, db | boost::adaptors::map_keys) {
                if (db.at(i).parent == handle)
                    handles.push_back(i);
            }
        }

        /*
         * getObjectPropList returns an ObjectPropList dataset table;
         * built as such:
         *
         * 1- Number of elements (quadruples)
         * a1- Element 1 Object Handle
         * a2- Element 1 Property Code
         * a3- Element 1 Data type
         * a4- Element 1 Value
         * b... rinse, repeat.
         */

        if (property == ALL_PROPERTIES)
             packet.putUInt32(6 * handles.size());
        else
             packet.putUInt32(1 * handles.size());

        BOOST_FOREACH(MtpObjectHandle i, handles) {
            DbEntry entry = db.at(i);

            // Persistent Unique Identifier.
            if (property == ALL_PROPERTIES || property == MTP_PROPERTY_PERSISTENT_UID) {
                packet.putUInt32(i);
                packet.putUInt16(MTP_PROPERTY_PERSISTENT_UID);
                packet.putUInt16(MTP_TYPE_UINT128);
                packet.putUInt128(i);
            }

            // Storage ID
            if (property == ALL_PROPERTIES || property == MTP_PROPERTY_STORAGE_ID) {
                packet.putUInt32(i);
                packet.putUInt16(MTP_PROPERTY_STORAGE_ID);
                packet.putUInt16(MTP_TYPE_UINT32);
                packet.putUInt32(entry.storage_id);
            }

            // Parent
            if (property == ALL_PROPERTIES || property == MTP_PROPERTY_PARENT_OBJECT) {
                packet.putUInt32(i);
                packet.putUInt16(MTP_PROPERTY_PARENT_OBJECT);
                packet.putUInt16(MTP_TYPE_UINT32);
                packet.putUInt32(entry.parent);
            }

            // Object Format
            if (property == ALL_PROPERTIES || property == MTP_PROPERTY_OBJECT_FORMAT) {
                packet.putUInt32(i);
                packet.putUInt16(MTP_PROPERTY_OBJECT_FORMAT);
                packet.putUInt16(MTP_TYPE_UINT16);
                packet.putUInt16(entry.object_format);
            }

            // Object Size
            if (property == ALL_PROPERTIES || property == MTP_PROPERTY_OBJECT_SIZE) {
                packet.putUInt32(i);
                packet.putUInt16(MTP_PROPERTY_OBJECT_SIZE);
                packet.putUInt16(MTP_TYPE_UINT64);
                packet.putUInt64(entry.object_size);
            }

            // Object File Name
            if (property == ALL_PROPERTIES || property == MTP_PROPERTY_OBJECT_FILE_NAME) {
                packet.putUInt32(i);
                packet.putUInt16(MTP_PROPERTY_OBJECT_FILE_NAME);
                packet.putUInt16(MTP_TYPE_STR);
                packet.putString(entry.display_name.c_str());
            }

            // Display Name
            if (property == ALL_PROPERTIES || property == MTP_PROPERTY_DISPLAY_NAME) {
                packet.putUInt32(i);
                packet.putUInt16(MTP_PROPERTY_DISPLAY_NAME);
                packet.putUInt16(MTP_TYPE_STR);
                packet.putString(entry.display_name.c_str());
            }

            // Association Type
            if (property == ALL_PROPERTIES || property == MTP_PROPERTY_ASSOCIATION_TYPE) {
                packet.putUInt32(i);
                packet.putUInt16(MTP_PROPERTY_ASSOCIATION_TYPE);
                packet.putUInt16(MTP_TYPE_UINT16);
                if (entry.object_format == MTP_FORMAT_ASSOCIATION)
                    packet.putUInt16(MTP_ASSOCIATION_TYPE_GENERIC_FOLDER);
                else
                    packet.putUInt16(0);
            }

            // Association Description
            if (property == ALL_PROPERTIES || property == MTP_PROPERTY_ASSOCIATION_DESC) {
                packet.putUInt32(i);
                packet.putUInt16(MTP_PROPERTY_ASSOCIATION_DESC);
                packet.putUInt16(MTP_TYPE_UINT32);
                packet.putUInt32(0);
            }

            // Protection Status
            if (property == ALL_PROPERTIES || property == MTP_PROPERTY_PROTECTION_STATUS) {
                packet.putUInt32(i);
                packet.putUInt16(MTP_PROPERTY_PROTECTION_STATUS);
                packet.putUInt16(MTP_TYPE_UINT16);
                packet.putUInt16(0x0000); //FIXME: all files are read-write for now
                // packet.putUInt16(0x8001);
            }

            // Date Created
            if (property == ALL_PROPERTIES || property == MTP_PROPERTY_DATE_CREATED) {
                char date[20];
                formatDateTime(0, date, sizeof(date));
                packet.putUInt32(i);
                packet.putUInt16(MTP_PROPERTY_DATE_CREATED);
                packet.putUInt16(MTP_TYPE_STR);
                packet.putString(date);
            }

            // Date Modified
            if (property == ALL_PROPERTIES || property == MTP_PROPERTY_DATE_MODIFIED) {
                char date[20];
                formatDateTime(entry.last_modified, date, sizeof(date));
                packet.putUInt32(i);
                packet.putUInt16(MTP_PROPERTY_DATE_CREATED);
                packet.putUInt16(MTP_TYPE_STR);
                packet.putString(date);
            }

            // Hidden
            if (property == ALL_PROPERTIES || property == MTP_PROPERTY_HIDDEN) {
                packet.putUInt32(i);
                packet.putUInt16(MTP_PROPERTY_HIDDEN);
                packet.putUInt16(MTP_TYPE_UINT16);
                packet.putUInt16(0);
            }

            // Non Consumable
            if (property == ALL_PROPERTIES || property == MTP_PROPERTY_NON_CONSUMABLE) {
                packet.putUInt32(i);
                packet.putUInt16(MTP_PROPERTY_NON_CONSUMABLE);
                packet.putUInt16(MTP_TYPE_UINT16);
                if (entry.object_format == MTP_FORMAT_ASSOCIATION)
                    packet.putUInt16(0); // folders are non-consumable
                else
                    packet.putUInt16(1); // files can usually be played.
                break;
            }
        }

        return MTP_RESPONSE_OK;
    }

    virtual MtpResponseCode getObjectInfo(
        MtpObjectHandle handle,
        MtpObjectInfo& info)
    {
        VLOG(2) << __PRETTY_FUNCTION__;

        if (handle == 0 || handle == MTP_PARENT_ROOT)
            return MTP_RESPONSE_INVALID_OBJECT_HANDLE;

        try {
            uint64_t object_size = db.at(handle).object_size;

            info.mHandle = handle;
            info.mStorageID = db.at(handle).storage_id;
            info.mFormat = db.at(handle).object_format;
            info.mProtectionStatus = 0x0;
            if (object_size > UINT64_C(0xFFFFFFFF))
                info.mCompressedSize = UINT64_C(0xFFFFFFFF);
            else
                info.mCompressedSize = object_size;
            info.mImagePixWidth = 0;
            info.mImagePixHeight = 0;
            info.mImagePixDepth = 0;
            info.mParent = db.at(handle).parent;
            info.mAssociationType
                = info.mFormat == MTP_FORMAT_ASSOCIATION
                    ? MTP_ASSOCIATION_TYPE_GENERIC_FOLDER : 0;
            info.mAssociationDesc = 0;
            info.mSequenceNumber = 0;
            info.mName = ::strdup(db.at(handle).display_name.c_str());
            info.mDateCreated = 0;
            info.mDateModified = db.at(handle).last_modified;
            info.mKeywords = ::strdup("MTP");

            if (VLOG_IS_ON(2))
                info.print();

            return MTP_RESPONSE_OK;
        }
        catch (...) {
            return MTP_RESPONSE_GENERAL_ERROR;
        }
    }

    virtual void* getThumbnail(MtpObjectHandle /*handle*/, size_t& outThumbSize)
    {
        outThumbSize = 0;
        return nullptr;
    }

    virtual MtpResponseCode getObjectFilePath(
        MtpObjectHandle handle,
        MtpString& outFilePath,
        int64_t& outFileLength,
        MtpObjectFormat& outFormat)
    {
        VLOG(1) << __PRETTY_FUNCTION__ << " handle: " << handle;

        if (handle == 0 || handle == MTP_PARENT_ROOT)
            return MTP_RESPONSE_INVALID_OBJECT_HANDLE;

        try {
            DbEntry entry = db.at(handle);

            VLOG(2) << __PRETTY_FUNCTION__
                    << "handle: " << handle
                    << "path: " << entry.path
                    << "length: " << entry.object_size
                    << "format: " << entry.object_format;

            outFilePath = std::string(entry.path);
            outFileLength = entry.object_size;
            outFormat = entry.object_format;

            return MTP_RESPONSE_OK;
        }
        catch (...) {
            return MTP_RESPONSE_GENERAL_ERROR;
        }
    }

    virtual MtpResponseCode deleteFile(MtpObjectHandle handle)
    {
        size_t orig_size = db.size();
        size_t new_size;

        VLOG(2) << __PRETTY_FUNCTION__ << " handle: " << handle;

        if (handle == 0 || handle == MTP_PARENT_ROOT)
            return MTP_RESPONSE_INVALID_OBJECT_HANDLE;

        try {
            if (db.at(handle).object_format == MTP_FORMAT_ASSOCIATION && db.at(handle).watch_fd >= 0)
                inotify_rm_watch(inotify_fd, db.at(handle).watch_fd);

            new_size = db.erase(handle);

            if (orig_size > new_size) {
                /* Recursively remove children object from the DB as well.
                 * we can safely ignore failures here, since the objects
                 * would not be reachable anyway.
                 */
                std::vector<MtpObjectHandle> to_erase;
                to_erase.reserve(64);
                for (std::map<MtpObjectHandle, DbEntry>::const_iterator it = db.begin(); it != db.end(); ++it) {
                    if (it->second.parent == handle)
                        to_erase.push_back(it->first);
                }
                for (size_t i = 0; i < to_erase.size(); ++i) {
                    db.erase(to_erase[i]);
                }

                return MTP_RESPONSE_OK;
            }
            else
                return MTP_RESPONSE_GENERAL_ERROR;
        }
        catch (...) {
            return MTP_RESPONSE_GENERAL_ERROR;
        }
    }

    virtual MtpResponseCode moveFile(MtpObjectHandle handle, MtpObjectHandle new_parent)
    {
        VLOG(1) << __PRETTY_FUNCTION__ << " handle: " << handle
                << " new parent: " << new_parent;

        if (handle == 0 || handle == MTP_PARENT_ROOT)
            return MTP_RESPONSE_INVALID_OBJECT_HANDLE;

        try {
            // change parent
            db.at(handle).parent = new_parent;
        }
        catch (...) {
            return MTP_RESPONSE_INVALID_OBJECT_HANDLE;
        }

        return MTP_RESPONSE_OK;
    }

    virtual MtpObjectHandleList* getObjectReferences(MtpObjectHandle handle)
    {
        VLOG(1) << __PRETTY_FUNCTION__;

        if (handle == 0 || handle == MTP_PARENT_ROOT)
            return nullptr;

        return getObjectList(db.at(handle).storage_id,
                             handle,
                             db.at(handle).object_format);
    }

    virtual MtpResponseCode setObjectReferences(
        MtpObjectHandle /*handle*/,
        MtpObjectHandleList* /*references*/)
    {
        VLOG(1) << __PRETTY_FUNCTION__;

        // ignore, we don't keep the references in a list.

        return MTP_RESPONSE_OK;
    }

    virtual MtpProperty* getObjectPropertyDesc(
        MtpObjectProperty property,
        MtpObjectFormat format)
    {
        VLOG(1) << __PRETTY_FUNCTION__ << MtpDebug::getObjectPropCodeName(property);

        (void)format;
        MtpProperty* result = nullptr;
        switch(property)
        {
            case MTP_PROPERTY_STORAGE_ID: result = new MtpProperty(property, MTP_TYPE_UINT32, false); break;
            case MTP_PROPERTY_PARENT_OBJECT: result = new MtpProperty(property, MTP_TYPE_UINT32, true); break;
            case MTP_PROPERTY_OBJECT_FORMAT: result = new MtpProperty(property, MTP_TYPE_UINT16, false); break;
            case MTP_PROPERTY_OBJECT_SIZE: result = new MtpProperty(property, MTP_TYPE_UINT64, false); break;
            case MTP_PROPERTY_WIDTH: result = new MtpProperty(property, MTP_TYPE_UINT32, false); break;
            case MTP_PROPERTY_HEIGHT: result = new MtpProperty(property, MTP_TYPE_UINT32, false); break;
            case MTP_PROPERTY_IMAGE_BIT_DEPTH: result = new MtpProperty(property, MTP_TYPE_UINT32, false); break;
            case MTP_PROPERTY_DISPLAY_NAME: result = new MtpProperty(property, MTP_TYPE_STR, true); break;
            case MTP_PROPERTY_OBJECT_FILE_NAME: result = new MtpProperty(property, MTP_TYPE_STR, true); break;
            case MTP_PROPERTY_PERSISTENT_UID: result = new MtpProperty(property, MTP_TYPE_UINT128, false); break;
            case MTP_PROPERTY_ASSOCIATION_TYPE: result = new MtpProperty(property, MTP_TYPE_UINT16, false); break;
            case MTP_PROPERTY_ASSOCIATION_DESC: result = new MtpProperty(property, MTP_TYPE_UINT32, false); break;
            case MTP_PROPERTY_PROTECTION_STATUS: result = new MtpProperty(property, MTP_TYPE_UINT16, false); break;
            case MTP_PROPERTY_DATE_CREATED: result = new MtpProperty(property, MTP_TYPE_STR, false); break;
            case MTP_PROPERTY_DATE_MODIFIED: result = new MtpProperty(property, MTP_TYPE_STR, false); break;
            case MTP_PROPERTY_HIDDEN: result = new MtpProperty(property, MTP_TYPE_UINT16, false); break;
            case MTP_PROPERTY_NON_CONSUMABLE: result = new MtpProperty(property, MTP_TYPE_UINT16, false); break;
            default: break;
        }

        return result;
    }

    virtual MtpProperty* getDevicePropertyDesc(MtpDeviceProperty property)
    {
        VLOG(1) << __PRETTY_FUNCTION__ << MtpDebug::getDevicePropCodeName(property);

        MtpProperty* result = nullptr;
        switch(property)
        {
            case MTP_DEVICE_PROPERTY_SYNCHRONIZATION_PARTNER:
            case MTP_DEVICE_PROPERTY_DEVICE_FRIENDLY_NAME:
                result = new MtpProperty(property, MTP_TYPE_STR, false); break;
            default: break;
        }

        return result;
    }

    virtual void sessionStarted(MtpServer* server)
    {
        VLOG(1) << __PRETTY_FUNCTION__;
        local_server = server;
    }

    virtual void sessionEnded()
    {
        VLOG(1) << __PRETTY_FUNCTION__;
        VLOG(1) << "objects in db at session end: " << db.size();
        local_server = nullptr;
    }
};
}

#endif // STUB_MTP_DATABASE_H_
