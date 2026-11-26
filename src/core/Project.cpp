#include "core/Project.h"
#include "core/Process.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <queue>
#include <set>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>

namespace kiri {
namespace fs=std::filesystem;
namespace {
std::string Lower(std::string s) {
    for(auto& c:s) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}
DirectoryResult ListDirectory(const std::string& path, const std::atomic<bool>* cancel) {
    DirectoryResult result;
    DIR* directory=opendir(path.c_str());
    if(!directory) { result.error=std::error_code(errno,std::generic_category()).message();return result; }
    auto base=path;if(base.empty() || base.back()!='/') base+='/';
    while(!(cancel && cancel->load())) {
        errno=0;auto* entry=readdir(directory);
        if(!entry) { if(errno) result.error=std::error_code(errno,std::generic_category()).message();break; }
        std::string name=entry->d_name;if(name=="." || name=="..") continue;
        DirectoryEntry item;item.name=name;item.path=base+name;
        // Haiku's dirent has no type field, so this lstat is the only
        // metadata read for ordinary entries; symlinks also check the target.
        struct stat info;
        if(lstat(item.path.c_str(),&info)==0) {
            item.stamp={uint64_t(info.st_dev),uint64_t(info.st_ino),uint64_t(info.st_size),info.st_mtim.tv_sec,info.st_mtim.tv_nsec,true};
            item.symlink=S_ISLNK(info.st_mode);
            if(item.symlink) { struct stat target;item.directory=stat(item.path.c_str(),&target)==0 && S_ISDIR(target.st_mode); }
            else { item.directory=S_ISDIR(info.st_mode);item.regular=S_ISREG(info.st_mode); }
        }
        result.entries.push_back(std::move(item));
    }
    closedir(directory);
    std::sort(result.entries.begin(),result.entries.end(),[](const auto& a,const auto& b) {
        if(a.directory!=b.directory) return a.directory;
        auto al=Lower(a.name),bl=Lower(b.name);
        return al==bl ? a.name<b.name : al<bl;
    });
    return result;
}
ProjectIndex IndexProject(const std::string& root, const std::atomic<bool>* cancel, size_t limit,bool includeIgnored,const IndexProgress& progress) {
    ProjectIndex result;
    auto cancelled=[&]{return cancel && cancel->load();};
    ProcessOptions opts;opts.directory=root;opts.cancel=cancel;opts.outputLimit=64*1024*1024;
    std::vector<std::string> command={"git","ls-files","-z","--cached","--others"};
    if(!includeIgnored) command.push_back("--exclude-standard");
    auto git=RunProcess(command,opts);
    if(git.ok()) {
        size_t offset=0;
        while(offset<git.output.size() && !cancelled()) {
            auto end=git.output.find('\0',offset);
            if(end==std::string::npos) break;
            if(result.paths.size()>=limit) { result.truncated=true;break; }
            auto path=git.output.substr(offset,end-offset);offset=end+1;
            std::error_code error;auto status=fs::symlink_status(fs::path(root)/path,error);
            if(!error && (fs::is_regular_file(status) || fs::is_symlink(status))) result.paths.push_back(std::move(path));
        }
        std::sort(result.paths.begin(),result.paths.end());
        result.paths.erase(std::unique(result.paths.begin(),result.paths.end()),result.paths.end());
        return result;
    }
    if(cancelled()) return result;
    const std::set<std::string> excluded={".git",".hg",".svn","node_modules",".cache",".vm","__pycache__",".venv","vendor","build","dist","target"};
    std::vector<fs::path> pending{root};
    using Clock=std::chrono::steady_clock;auto report=Clock::now()+std::chrono::milliseconds(500);
    auto base=root;if(base.empty() || base.back()!='/') base+='/';
    while(!pending.empty() && !cancelled()) {
        auto current=pending.back();pending.pop_back();
        auto listing=ListDirectory(current.string(),cancel);
        if(!listing.error.empty()) { if(current==fs::path(root)) result.error=listing.error;continue; }
        for(const auto& entry:listing.entries) {
            if(entry.symlink) continue; // no cycles, escaping project, or device reads during indexing
            if(entry.directory) { if(entry.name!=".git" && entry.name!=".hg" && entry.name!=".svn" && (includeIgnored || !excluded.count(entry.name))) pending.emplace_back(entry.path); }
            else if(entry.regular) { // named pipes, sockets and devices are not project files
                if(result.paths.size()>=limit) { result.truncated=true;return result; }
                result.paths.push_back(entry.path.compare(0,base.size(),base)==0?entry.path.substr(base.size()):fs::path(entry.path).lexically_relative(root).string());
            }
        }
        if(progress && !pending.empty() && !cancelled() && Clock::now()>=report) {
            result.partial=true;progress(result);result.partial=false;
            report=Clock::now()+std::chrono::seconds(2);
        }
    }
    return result;
}
namespace {
// One code point from UTF-8; a malformed byte stands for itself.
uint32_t NextCharacter(const std::string& text,size_t& at) {
    unsigned char c=text[at++];
    if(c<0x80) return c;
    size_t length=c>=0xf0?3:c>=0xe0?2:c>=0xc0?1:0;
    if(!length || at+length>text.size()) return c;
    uint32_t value=c&(0x3f>>length);
    for(size_t i=0;i<length;++i) {
        unsigned char next=text[at+i];if((next&0xc0)!=0x80) return c;
        value=(value<<6)|(next&0x3f);
    }
    at+=length;return value;
}
// Simple case folding for the scripts file names commonly use: Latin
// (including Latin-1, Extended-A and Extended Additional), Greek and Cyrillic.
uint32_t FoldCase(uint32_t c) {
    if(c<0x80) return c>='A' && c<='Z'?c+32:c;
    if((c>=0xc0 && c<=0xde && c!=0xd7)) return c+32;
    if(c>=0x100 && c<=0x17f) {
        if(c==0x130) return 'i';
        if(c==0x178) return 0xff;
        if((c>=0x139 && c<=0x148) || (c>=0x179 && c<=0x17e)) return c&1?c+1:c;
        if(c!=0x138 && c!=0x149 && c!=0x17f) return c&1?c:c+1;
        return c;
    }
    if(c==0x386) return 0x3ac;
    if(c>=0x388 && c<=0x38a) return c+37;
    if(c==0x38c) return 0x3cc;
    if(c==0x38e || c==0x38f) return c+63;
    if(c>=0x391 && c<=0x3ab && c!=0x3a2) return c+32;
    if(c==0x3c2) return 0x3c3;
    if(c>=0x400 && c<=0x40f) return c+80;
    if(c>=0x410 && c<=0x42f) return c+32;
    if(((c>=0x460 && c<=0x481) || (c>=0x48a && c<=0x4bf) || (c>=0x4d0 && c<=0x52f)) && !(c&1)) return c+1;
    if(c>=0x1e00 && c<=0x1eff && !(c>=0x1e96 && c<=0x1e9f) && !(c&1)) return c+1;
    return c;
}
}
int FuzzyScore(const std::string& query, const std::string& path) {
    if(query.empty()) return -static_cast<int>(path.size());
    size_t pos=0,previousEnd=SIZE_MAX;int score=0;
    for(size_t q=0;q<query.size();) {
        auto wanted=NextCharacter(query,q),folded=FoldCase(wanted);bool found=false;
        while(pos<path.size()) {
            auto start=pos;auto c=NextCharacter(path,pos);
            if(c!=wanted && FoldCase(c)!=folded) continue;
            score+=10;
            if(start==previousEnd) score+=12;
            if(start==0 || path[start-1]=='/' || path[start-1]=='_' || path[start-1]=='-') score+=20;
            if(c==wanted) score+=2;
            previousEnd=pos;found=true;break;
        }
        if(!found) return -1000000;
    }
    return score-static_cast<int>(path.size());
}
std::vector<std::string> QuickOpen(const ProjectIndex& index,const std::string& query,size_t limit) {
    using Candidate=std::pair<int,size_t>;
    std::priority_queue<Candidate,std::vector<Candidate>,std::greater<Candidate>> best;
    for(size_t i=0;i<index.paths.size();++i) {
        int score=FuzzyScore(query,index.paths[i]);
        if(score<=-1000000) continue;
        best.emplace(score,i);if(best.size()>limit) best.pop();
    }
    std::vector<std::string> result(best.size());
    for(size_t n=best.size();n>0;--n) { result[n-1]=index.paths[best.top().second];best.pop(); }
    return result;
}

namespace {
std::vector<std::string> Patterns(const std::string& text) {
    std::vector<std::string> parts;
    for(size_t start=0;start<text.size();) {
        auto end=text.find(';',start);if(end==std::string::npos) end=text.size();
        auto part=text.substr(start,end-start);auto first=part.find_first_not_of(" \t");
        if(first!=std::string::npos) parts.push_back(part.substr(first,part.find_last_not_of(" \t")-first+1));
        start=end+1;
    }return parts;
}
// * spans directory separators; each **/ additionally matches zero
// directories. Every **/ from `from` on is tried both ways, independently,
// so a/**/b/**/c matches a/x/b/c (first spans x/, second spans nothing).
bool RecursiveGlob(const std::string& pattern,const std::string& path,size_t from) {
    auto recursive=pattern.find("**/",from);
    if(recursive==std::string::npos) return fnmatch(pattern.c_str(),path.c_str(),0)==0;
    return RecursiveGlob(pattern,path,recursive+3)
        || RecursiveGlob(pattern.substr(0,recursive)+pattern.substr(recursive+3),path,recursive);
}
bool Glob(const std::string& pattern,const std::string& path) {
    if(RecursiveGlob(pattern,path,0)) return true;
    return pattern.find('/')==std::string::npos && fnmatch(pattern.c_str(),fs::path(path).filename().c_str(),0)==0;
}
}
bool MatchesProjectPath(const std::string& path,const ProjectSearchOptions& options) {
    auto folders=Patterns(options.folders),include=Patterns(options.include),exclude=Patterns(options.exclude);
    bool scoped=folders.empty();
    for(auto folder:folders) {
        while(!folder.empty() && folder.back()=='/') folder.pop_back();
        if(folder.empty() || folder=="." || path==folder || path.rfind(folder+"/",0)==0) scoped=true;
    }
    if(!scoped) return false;
    if(!include.empty() && std::none_of(include.begin(),include.end(),[&](const auto& p){return Glob(p,path);})) return false;
    return std::none_of(exclude.begin(),exclude.end(),[&](const auto& p){return Glob(p,path);});
}
std::string SearchResult::Summary() const {
    if(!error.empty()) return error;
    return std::to_string(matches.size())+" matches"+(truncated?" (result limit reached)":"")+(cancelled?" · cancelled":"")
        +" · skipped: "+std::to_string(binary)+" binary/encoding, "+std::to_string(large)+" size, "
        +std::to_string(unreadable)+" unreadable, "+std::to_string(symlinks)+" symlinks, "+std::to_string(filtered)+" filtered";
}
SearchResult SearchProject(const std::string& root,const ProjectIndex& index,const std::string& query,
    bool matchCase,const std::atomic<bool>* cancel,size_t limit) {
    ProjectSearchOptions options;options.query=query;options.matchCase=matchCase;
    return SearchProject(root,index,options,cancel,limit);
}
SearchResult SearchProject(const std::string& root,const ProjectIndex& index,const ProjectSearchOptions& options,
    const std::atomic<bool>* cancel,size_t limit,const std::map<std::string,FileData>* buffers,const std::string* replacement) {
    SearchResult result;TextQuery query(options);result.error=replacement?query.ValidateReplacement(*replacement):query.Error();
    if(!result.error.empty() || options.query.empty()) return result;
    std::set<std::string> paths(index.paths.begin(),index.paths.end());
    // Component-wise containment: for the root folder / the prefix is "/", not "//".
    auto canonicalRoot=CanonicalPath(root);if(canonicalRoot.empty() || canonicalRoot.back()!='/') canonicalRoot+='/';
    // The index decides ignored-file inclusion. Unsaved buffers replace the
    // contents of indexed files without silently widening the selected scope.
    size_t snapshotBytes=0;
    for(const auto& relative:paths) {
        if(cancel && cancel->load()) { result.cancelled=true;break; }
        if(!MatchesProjectPath(relative,options)) { ++result.filtered;continue; }
        fs::path path=fs::path(root)/relative;std::error_code error;
        if(fs::path(relative).is_absolute() || relative==".." || relative.rfind("../",0)==0 || CanonicalPath(path.string()).rfind(canonicalRoot,0)!=0
                || fs::is_symlink(path,error)) { ++result.symlinks;++result.skipped;continue; }
        FileData data;bool fromBuffer=buffers && buffers->count(relative);
        if(fromBuffer) data=buffers->at(relative);
        else {
            auto size=fs::file_size(path,error);
            if(!error && size>32*1024*1024) { ++result.large;++result.skipped;continue; }
            data=ReadFile(path.string(),cancel,32*1024*1024);
        }
        if(!data.ok()) { ++result.unreadable;++result.skipped;continue; }
        if(data.binary || !data.utf8) { ++result.binary;++result.skipped;continue; }
        if(!fromBuffer && data.bom) data.bytes.erase(0,3);
        if(data.bytes.size()>32*1024*1024) { ++result.large;++result.skipped;continue; }
        size_t first=result.matches.size(),line=1;
        for(size_t start=0;start<=data.bytes.size();) {
            if(cancel && cancel->load()) { result.cancelled=true;break; }
            auto end=data.bytes.find_first_of("\r\n",start);if(end==std::string::npos) end=data.bytes.size();
            auto found=query.Find(data.bytes,start,end,limit-result.matches.size(),cancel,replacement);
            if(!found.error.empty()) { result.error=relative+": "+found.error;result.matches.clear();result.snapshots.clear();return result; }
            // Columns advance with the matches: each byte of the line is
            // decoded once however many matches it holds (valid UTF-8 here).
            size_t columnByte=start,column=1;
            for(auto& match:found.matches) {
                if(cancel && (result.matches.size()&1023)==0 && cancel->load()) { result.cancelled=true;break; }
                for(;columnByte<match.start;++columnByte) column+=(static_cast<unsigned char>(data.bytes[columnByte])&0xc0)!=0x80;
                auto begin=match.start>start+120?match.start-120:start;
                while(begin>start && (static_cast<unsigned char>(data.bytes[begin])&0xc0)==0x80) --begin;
                auto excerptEnd=std::min(end,begin+400);
                while(excerptEnd<end && (static_cast<unsigned char>(data.bytes[excerptEnd])&0xc0)==0x80) ++excerptEnd;
                result.matches.push_back({relative,data.bytes.substr(begin,excerptEnd-begin),line,column,match.start,match.end,std::move(match.text)});
            }
            if(result.cancelled) break;
            if(found.truncated) { result.truncated=true;break; }
            if(found.cancelled) { result.cancelled=true;break; }
            if(end==data.bytes.size()) break;
            start=end+1;if(data.bytes[end]=='\r' && start<data.bytes.size() && data.bytes[start]=='\n') ++start;
            ++line;
        }
        if(replacement && first!=result.matches.size()) {
            snapshotBytes+=data.bytes.size();
            if(snapshotBytes>128*1024*1024) { result.error="Replacement preview exceeds 128 MiB. Narrow the folders or filters.";result.matches.clear();result.snapshots.clear();return result; }
            result.snapshots.emplace(relative,std::move(data));
        }
        if(result.truncated || result.cancelled) break;
    }
    return result;
}
}
