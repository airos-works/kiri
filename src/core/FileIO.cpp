#include "core/FileIO.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __HAIKU__
#include <fs_attr.h>
#include <fs_info.h>
#include <dirent.h>
#endif

namespace kiri {
namespace {
class UTF8Validator {
public:
    void Feed(std::string_view bytes) {
        for(unsigned char c:bytes) {
            if(!valid) return;
            if(remaining) {
                if((c&0xc0)!=0x80) { valid=false;return; }
                code=(code<<6)|(c&63);
                if(--remaining==0 && (code<minimum || code>0x10ffff || (code>=0xd800 && code<=0xdfff))) valid=false;
            } else if(c<0x80) continue;
            else if(c>=0xc2 && c<=0xdf) { remaining=1;code=c&31;minimum=0x80; }
            else if(c>=0xe0 && c<=0xef) { remaining=2;code=c&15;minimum=0x800; }
            else if(c>=0xf0 && c<=0xf4) { remaining=3;code=c&7;minimum=0x10000; }
            else valid=false;
        }
    }
    bool Complete() const { return valid && remaining==0; }
private: bool valid=true;int remaining=0;uint32_t code=0,minimum=0;
};
FileStamp Stamp(const struct stat& s) {
    return {static_cast<uint64_t>(s.st_dev),static_cast<uint64_t>(s.st_ino),
        static_cast<uint64_t>(s.st_size),s.st_mtim.tv_sec,s.st_mtim.tv_nsec,true};
}
std::string Error(const char* action) { return std::string(action)+": "+strerror(errno); }
// Opens without blocking on a pipe, socket or device and without making a
// terminal the controlling one. Returns -2 when the path is not a regular
// file, else the descriptor (in blocking mode) or -1 with errno set.
int OpenRegular(const std::string& path,int flags) {
    int fd=open(path.c_str(),flags|O_CLOEXEC|O_NONBLOCK|O_NOCTTY);
    if (fd<0) return -1;
    struct stat s{};
    if (fstat(fd,&s)!=0) { int saved=errno;close(fd);errno=saved;return -1; }
    if (!S_ISREG(s.st_mode)) { close(fd);return -2; }
    int mode=fcntl(fd,F_GETFL);if(mode!=-1) fcntl(fd,F_SETFL,mode&~O_NONBLOCK);
    return fd;
}
#ifdef __HAIKU__
// A volume that cannot store attributes (an SMB share, for one) may still show
// some it makes up, such as a MIME type from the file's name. Those are not the
// file's to keep, and writing them back fails as a read-only file system.
bool StoresAttributes(int fd) {
    struct stat s{};fs_info info{};
    return fstat(fd,&s)!=0 || fs_stat_dev(s.st_dev,&info)!=0 || (info.flags&B_FS_HAS_ATTR)!=0;
}
bool CopyAttributes(int source, int target) {
    if (!StoresAttributes(target)) return true;
    DIR* attrs=fs_fopen_attr_dir(source);
    if (!attrs) return false;
    bool ok=true;
    while (auto* entry=fs_read_attr_dir(attrs)) {
        attr_info info;
        if (fs_stat_attr(source,entry->d_name,&info)!=0) { ok=false; break; }
        char buffer[65536];
        off_t offset=0;
        while (offset<info.size) {
            ssize_t n=fs_read_attr(source,entry->d_name,info.type,offset,buffer,
                std::min<off_t>(sizeof(buffer),info.size-offset));
            if (n<=0 || fs_write_attr(target,entry->d_name,info.type,offset,buffer,n)!=n) { ok=false; break; }
            offset+=n;
        }
        if (!ok) break;
    }
    fs_close_attr_dir(attrs);
    return ok;
}
#endif
}
bool FileStamp::operator==(const FileStamp& o) const {
    return exists==o.exists && (!exists || (device==o.device && inode==o.inode && size==o.size
        && seconds==o.seconds && nanoseconds==o.nanoseconds));
}
std::function<void(const std::string&)>& MetadataProbe() { static std::function<void(const std::string&)> probe;return probe; }
FileStamp StatFile(const std::string& path) { if(auto& probe=MetadataProbe()) probe(path);struct stat s; return stat(path.c_str(),&s)==0 ? Stamp(s) : FileStamp{}; }
std::string CanonicalPath(const std::string& path) {
    if(auto& probe=MetadataProbe()) probe(path);
    std::error_code error;
    auto resolved=std::filesystem::weakly_canonical(std::filesystem::absolute(path,error),error);
    return error ? path : resolved.string();
}
bool IsValidUTF8(std::string_view bytes) {
    UTF8Validator validator;validator.Feed(bytes);return validator.Complete();
}
FileData ReadFile(const std::string& path, const std::atomic<bool>* cancel, size_t limit,
    const std::function<bool(std::string_view)>& sink) {
    FileData data;
    // Nonblocking, so a named pipe or device cannot hold the caller in
    // open(2) before the type check below; regular files then read normally.
    int fd=OpenRegular(path,O_RDONLY);
    if (fd==-2) { data.error="This is not a regular file.";return data; }
    if (fd<0) { data.error=Error("Cannot open file");return data; }
    struct stat s{};
    if (fstat(fd,&s)!=0) { data.error=Error("Cannot inspect file");close(fd);return data; }
    data.stamp=Stamp(s);
    // Most binary formats identify themselves immediately. Keep their preview
    // bounded even when the file is much larger than the text editing limit.
    char sniff[65536];
    ssize_t sniffSize=pread(fd,sniff,sizeof(sniff),0);
    if(sniffSize>0 && std::find(sniff,sniff+sniffSize,'\0')!=sniff+sniffSize) {
        data.binary=true;data.utf8=IsValidUTF8(std::string_view(sniff,sniffSize));
        data.bytes.assign(sniff,sniffSize);
        struct stat after{};if(fstat(fd,&after)!=0 || Stamp(after)!=data.stamp) data.error="File changed while reading. Open it again.";
        close(fd);return data;
    }
    if (data.stamp.size>limit) { data.error="The file exceeds the current 1 GiB editing limit.";close(fd);return data; }
    std::string block;
    try { if(sink) { block.resize(1024*1024);if(sniffSize>0) data.bytes.assign(sniff,sniffSize); } else data.bytes.resize(data.stamp.size); }
    catch (...) { data.error="Not enough memory to open this file.";close(fd);return data; }
    size_t offset=0;
    UTF8Validator validator;bool foundEOL=false,pendingCR=false;
    data.bom=sniffSize>=3 && std::string_view(sniff,3)=="\xef\xbb\xbf";
    while (offset<data.stamp.size) {
        if (cancel && cancel->load()) { data.error="Open cancelled.";break; }
        char* destination=sink?block.data():data.bytes.data()+offset;
        ssize_t n=read(fd,destination,std::min<uint64_t>(1024*1024,data.stamp.size-offset));
        if (n>0) {
            std::string_view chunk(destination,n);validator.Feed(chunk);
            data.binary=data.binary || chunk.find('\0')!=std::string_view::npos;
            if(pendingCR) { data.eol=chunk.front()=='\n'?0:1;pendingCR=false; }
            if(!foundEOL) {
                auto eol=chunk.find_first_of("\r\n");
                if(eol!=std::string_view::npos) { foundEOL=true;
                    if(chunk[eol]=='\r') { data.eol=1;pendingCR=eol+1==chunk.size();if(!pendingCR && chunk[eol+1]=='\n') data.eol=0; }
                }
            }
            if(sink) {
                auto content=chunk;if(offset==0 && data.bom) content.remove_prefix(3);
                if(!sink(content)) { data.error="Not enough memory to load this document.";break; }
            }
            offset+=n;
        }
        else if (n<0 && errno==EINTR) continue;
        else { data.error=n==0 ? "File changed while reading. Open it again." : Error("Cannot read file");break; }
    }
    struct stat after{};
    if (fstat(fd,&after)!=0 || Stamp(after)!=data.stamp) data.error="File changed while reading. Open it again.";
    close(fd);
    if (!data.ok()) { data.bytes.clear();return data; }
    data.utf8=validator.Complete();
    return data;
}
namespace {
std::string WriteParts(int fd,const std::vector<std::string_view>& parts) {
    for(auto bytes:parts) {
        size_t offset=0;
        while (offset<bytes.size()) {
            ssize_t n=write(fd,bytes.data()+offset,std::min<size_t>(1024*1024,bytes.size()-offset));
            if (n>0) offset+=n;
            else if (n<0 && errno==EINTR) continue;
            else return Error("Cannot write file");
        }
    }
    return {};
}
// Returns an error and sets damaged when the destination was truncated but
// not completely rewritten; the caller then keeps its complete copy.
std::string OverwriteInPlace(const std::string& path,const std::vector<std::string_view>& parts,const FileStamp& expected,bool& damaged) {
    damaged=false;
    if (!expected.exists) return "Cannot replace file: it is in use.";
    auto& faults=SaveFaultInjection();if(faults.beforeOverwrite) faults.beforeOverwrite(path);
    // Truncate only after the opened file proves to be the one we read:
    // O_TRUNC would empty a file somebody else just put at this path.
    int fd=open(path.c_str(),O_WRONLY|O_CLOEXEC|O_NONBLOCK|O_NOCTTY);
    if (fd<0) return Error("Cannot replace file");
    struct stat opened{};
    if (fstat(fd,&opened)!=0 || !S_ISREG(opened.st_mode) || Stamp(opened)!=expected) { close(fd);return "The file changed during save. It was not modified."; }
    int mode=fcntl(fd,F_GETFL);if(mode!=-1) fcntl(fd,F_SETFL,mode&~O_NONBLOCK);
    if (ftruncate(fd,0)!=0) { auto error=Error("Cannot replace file");close(fd);return error; }
    damaged=true;
    std::string error;
    if (faults.writeErrno) { errno=faults.writeErrno;error=Error("Cannot write file"); }
    else error=WriteParts(fd,parts);
    if (error.empty() && faults.flushErrno) { errno=faults.flushErrno;error=Error("Cannot flush file"); }
    else if (error.empty() && fsync(fd)!=0) error=Error("Cannot flush file");
    if (close(fd)!=0 && error.empty()) error=Error("Cannot close file");
    if (error.empty()) damaged=false;
    return error;
}
}
SaveFaults& SaveFaultInjection() { static SaveFaults faults;return faults; }
std::string SaveFile(const std::string& inputPath, std::string_view bytes, const FileStamp& expected) {
    return SaveFileParts(inputPath,{bytes},expected);
}
std::string SaveFileParts(const std::string& inputPath,const std::vector<std::string_view>& parts,const FileStamp& expected) {
    std::string path=CanonicalPath(inputPath);
    if (StatFile(path)!=expected) return "The file changed on disk. Reload it or save your edits to a different file.";
    int old=-1;
    struct stat oldStat{};
    if (expected.exists) {
        old=OpenRegular(path,O_RDONLY);
        if (old==-2) return "This is not a regular file. Save to a different file.";
        if (old<0 || fstat(old,&oldStat)!=0) { if(old>=0) close(old);return Error("Cannot inspect original file"); }
        if (!(oldStat.st_mode&0222)) { close(old);return "The file is read-only. Save to a different file."; }
    }
    std::string name=path+".kiri-save-XXXXXX";
    int fd=mkstemp(name.data());
    if (fd<0) { if(old>=0) close(old);return Error("Cannot create temporary file"); }
    fcntl(fd,F_SETFD,FD_CLOEXEC);
    std::string error;
    if (old>=0 && fchmod(fd,oldStat.st_mode&0777)!=0) error=Error("Cannot preserve file permissions");
    // New documents retain mkstemp's private 0600 permissions. Existing files
    // keep their own permissions, including their executable bit.
#ifdef __HAIKU__
    if (old>=0 && error.empty() && !CopyAttributes(old,fd)) error=Error("Cannot preserve file attributes");
#endif
    if (error.empty()) error=WriteParts(fd,parts);
    if (error.empty() && fsync(fd)!=0) error=Error("Cannot flush file");
    if (close(fd)!=0 && error.empty()) error=Error("Cannot close file");
    if (error.empty() && StatFile(path)!=expected) error="The file changed during save. It was not modified.";
    // An SMB server does not replace a file that is open, ours included.
    if (old>=0) { close(old);old=-1; }
    int renamed=0;
    if (error.empty()) {
        if (int injected=SaveFaultInjection().renameErrno) { errno=injected;renamed=-1; }
        else renamed=rename(name.c_str(),path.c_str());
    }
    if (error.empty() && renamed!=0) {
        // Somebody else has it open (Tracker reading its icon, a viewer).
        // Writing over it in place is what other editors do, and still saves.
        bool damaged=false;
        if (errno==EACCES || errno==EPERM || errno==EBUSY) error=OverwriteInPlace(path,parts,expected,damaged);
        else error=Error("Cannot replace file");
        // The temporary file is complete and flushed: when the original may
        // now be incomplete, it is the recoverable copy, so keep and name it.
        if (damaged) error+=". The file may be incomplete. A complete copy was kept at "+name;
        else unlink(name.c_str());
    } else if (!error.empty()) unlink(name.c_str());
    if (error.empty()) {
        auto parent=std::filesystem::path(path).parent_path().string();
        int directory=open(parent.c_str(),O_RDONLY|O_CLOEXEC);
        if (directory>=0) { fsync(directory);close(directory); }
    }
    return error;
}
std::string HexPreview(std::string_view bytes, uint64_t totalSize) {
    std::string result="Binary file — "+std::to_string(totalSize)+" bytes\n\n";
    const size_t limit=std::min<size_t>(bytes.size(),65536);
    char buffer[32];
    for (size_t offset=0;offset<limit;offset+=16) {
        snprintf(buffer,sizeof(buffer),"%08llx  ",static_cast<unsigned long long>(offset));result+=buffer;
        for(size_t i=0;i<16;++i) {
            if(offset+i<limit) snprintf(buffer,sizeof(buffer),"%02x ",static_cast<unsigned char>(bytes[offset+i]));
            else strcpy(buffer,"   ");
            result+=buffer;if(i==7) result+=' ';
        }
        result+=" |";
        for(size_t i=0;i<16 && offset+i<limit;++i) {
            unsigned char c=bytes[offset+i];result+=c>=32 && c<127 ? static_cast<char>(c) : '.';
        }
        result+="|\n";
    }
    if(totalSize>limit) result+="\nPreview limited to the first 64 KiB.\n";
    return result;
}
}
