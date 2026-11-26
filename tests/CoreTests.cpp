#include "core/FileIO.h"
#include "core/Git.h"
#include "core/Project.h"
#include "core/Terminal.h"
#include "core/Recovery.h"
#include "core/EditTransaction.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <signal.h>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace kiri;
namespace fs=std::filesystem;
static int checks=0;
#define CHECK(x) do { ++checks;if(!(x)) throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+": " #x); } while(false)
struct Temporary {
    std::string path;
    Temporary() { std::string pattern=(fs::temp_directory_path()/"kiri-tests-XXXXXX").string();char* p=mkdtemp(pattern.data());if(!p) throw std::runtime_error("mkdtemp");path=p; }
    ~Temporary() { std::error_code e;fs::remove_all(path,e); }
};
void Put(const fs::path& path,const std::string& data) { std::ofstream f(path,std::ios::binary);f.write(data.data(),data.size()); }
void Processes() {
    Temporary t;
    ProcessOptions options;options.directory=t.path;
    options.input=std::string("hello\0world",11);
    auto result=RunProcess({"cat"},options);
    CHECK(result.ok());CHECK(result.output==options.input);
    result=RunProcess({"printf","%s","$(touch INJECTED); 'quoted' \"word\""},options);
    CHECK(result.ok());CHECK(result.output=="$(touch INJECTED); 'quoted' \"word\"");CHECK(!fs::exists(fs::path(t.path)/"INJECTED"));
    options.timeout=std::chrono::milliseconds(100);
    auto start=std::chrono::steady_clock::now();
    result=RunProcess({"sleep","10"},options);
    CHECK(result.timedOut);CHECK(std::chrono::steady_clock::now()-start<std::chrono::seconds(2));
    options={};options.outputLimit=100;
    result=RunProcess({"sh","-c","while :; do printf 1234567890; done"},options);
    CHECK(result.truncated);CHECK(result.output.size()<=100);
    std::atomic<bool> cancel{true};options={};options.cancel=&cancel;
    result=RunProcess({"sleep","10"},options);CHECK(result.cancelled);
    result=RunProcess({"kiri-a-command-that-does-not-exist"});CHECK(result.exitCode==127);
    // A killed child is reaped within the bound (a blocked one is left to a reaper thread).
    auto child=fork();if(child==0) { pause();_exit(0); }
    CHECK(child>0);kill(child,SIGKILL);int status=0;start=std::chrono::steady_clock::now();
    CHECK(ReapKilledChild(child,status));CHECK(std::chrono::steady_clock::now()-start<std::chrono::seconds(1));
}
void Files() {
    Temporary t;auto path=fs::path(t.path)/"a 'file'.cpp";
    std::string bytes="\xef\xbb\xbf// café 日本語\r\nint main() {}\r\n";
    Put(path,bytes);chmod(path.c_str(),0750);
    auto data=ReadFile(path.string());CHECK(data.ok());CHECK(data.bytes==bytes);CHECK(data.utf8);CHECK(data.bom);CHECK(data.eol==0);CHECK(!data.binary);
    std::string streamed;
    auto stream=ReadFile(path.string(),nullptr,1024*1024,[&](std::string_view chunk){streamed.append(chunk);return true;});
    CHECK(stream.ok());CHECK(streamed==bytes.substr(3));CHECK(stream.bom);CHECK(stream.eol==0);
    CHECK(SaveFile(path.string(),bytes+"// changed\r\n",data.stamp).empty());
    struct stat s{};stat(path.c_str(),&s);CHECK((s.st_mode&0777)==0750);
    CHECK(!SaveFile(path.string(),"stale",data.stamp).empty());
    auto updated=ReadFile(path.string());CHECK(updated.bytes==bytes+"// changed\r\n");
    auto link=fs::path(t.path)/"link.cpp";fs::create_symlink(path,link);
    CHECK(SaveFile(link.string(),"via link\n",updated.stamp).empty());CHECK(fs::is_symlink(link));CHECK(ReadFile(path.string()).bytes=="via link\n");
    auto newPath=fs::path(t.path)/"new.json";CHECK(SaveFile(newPath.string(),"{}\n",{}).empty());
    CHECK(!SaveFile(newPath.string(),"overwrite",{}).empty());
    CHECK(!IsValidUTF8("\xc0\xaf"));CHECK(!IsValidUTF8("\xed\xa0\x80"));CHECK(!IsValidUTF8("\xf4\x90\x80\x80"));
    CHECK(IsValidUTF8("😀 日本語"));CHECK(!IsValidUTF8("\xe2\x82"));
    std::string boundary(1024*1024-1,'a');boundary+="😀\r\n";Put(path,boundary);streamed.clear();
    stream=ReadFile(path.string(),nullptr,2*1024*1024,[&](std::string_view chunk){streamed.append(chunk);return true;});
    CHECK(stream.ok());CHECK(stream.utf8);CHECK(streamed==boundary);CHECK(stream.bytes.size()==65536);CHECK(stream.eol==0);
    Put(path,std::string("a\0b",3));CHECK(ReadFile(path.string()).binary);
    int sparse=open(path.c_str(),O_WRONLY);CHECK(sparse>=0);CHECK(ftruncate(sparse,2LL*1024*1024*1024)==0);close(sparse);
    auto binary=ReadFile(path.string());CHECK(binary.ok());CHECK(binary.binary);CHECK(binary.stamp.size==2ULL*1024*1024*1024);CHECK(binary.bytes.size()==65536);
}
// A refused rename falls back to writing in place; if that fails, the
// complete temporary copy is kept and named (#16).
void InPlaceSaveFailures() {
    Temporary t;auto path=(fs::path(t.path)/"shared.txt").string();Put(path,"original\n");auto& faults=SaveFaultInjection();
    auto temporaries=[&]{std::vector<std::string> names;for(auto& entry:fs::directory_iterator(t.path)) if(entry.path().filename().string().find(".kiri-save-")!=std::string::npos) names.push_back(entry.path().string());return names;};
    faults.renameErrno=EBUSY;CHECK(SaveFile(path,"in place\n",StatFile(path)).empty());CHECK(ReadFile(path).bytes=="in place\n");CHECK(temporaries().empty());
    for(int fault:{1,2}) {
        Put(path,"original\n");(fault==1?faults.writeErrno:faults.flushErrno)=EIO;
        auto error=SaveFile(path,"complete edit\n",StatFile(path));CHECK(!error.empty());
        auto kept=temporaries();CHECK(kept.size()==1);CHECK(error.find(kept[0])!=std::string::npos);CHECK(error.find("editor")==std::string::npos);
        CHECK(ReadFile(kept[0]).bytes=="complete edit\n");fs::remove(kept[0]);faults.writeErrno=faults.flushErrno=0;
    }
    // Background project replacement of a closed file reports the same copy.
    Put(path,"old text\n");FileEdit edit;edit.before=DiskSnapshot(path);edit.edits.push_back({{0,3,"new"},true});edit.Prepare();faults.writeErrno=EIO;
    auto error=ApplyDiskEdit(edit);CHECK(!error.empty());auto kept=temporaries();CHECK(kept.size()==1);CHECK(error.find(kept[0])!=std::string::npos);
    CHECK(ReadFile(kept[0]).bytes=="new text\n");CHECK(!edit.applied);fs::remove(kept[0]);faults.writeErrno=0;
    // The opened file is checked before truncation: a replaced file is left alone.
    Put(path,"mine\n");faults.beforeOverwrite=[](const std::string& target){Put(target+".other","theirs, newer\n");fs::rename(target+".other",target);};
    error=SaveFile(path,"edit\n",StatFile(path));CHECK(!error.empty());CHECK(ReadFile(path).bytes=="theirs, newer\n");CHECK(temporaries().empty());
    faults=SaveFaults{};
}
// Named pipes are skipped by the index and never block a reader (#39).
void SpecialFiles() {
    Temporary t;Put(fs::path(t.path)/"plain.txt","needle\n");auto fifo=(fs::path(t.path)/"pipe.txt").string();CHECK(mkfifo(fifo.c_str(),0600)==0);
    alarm(20); // a blocking open would otherwise hang the suite
    auto index=IndexProject(t.path);CHECK(index.paths==std::vector<std::string>{"plain.txt"});
    auto listing=ListDirectory(t.path);for(auto& entry:listing.entries) CHECK(entry.regular==(entry.name=="plain.txt"));
    auto data=ReadFile(fifo);CHECK(!data.ok());CHECK(data.error=="This is not a regular file.");
    index.paths.push_back("pipe.txt");auto search=SearchProject(t.path,index,"needle",true);CHECK(search.matches.size()==1);CHECK(search.unreadable==1);
    CHECK(!SaveFile(fifo,"text",StatFile(fifo)).empty());
    alarm(0);
}
void LinksAndGraph() {
    for(const auto& remote:{"git@github.com:owner/repo.git","https://github.com/owner/repo.git","ssh://git@github.com/owner/repo.git","ssh://git@github.com:22/owner/repo.git"})
        CHECK(GitHubRepository(remote)=="https://github.com/owner/repo");
    CHECK(!GitHubRepository("https://github.com.evil.test/owner/repo.git"));
    CHECK(!GitHubRepository("git@gitlab.com:owner/repo.git"));CHECK(!GitHubRepository("https://github.com/../repo"));
    CHECK(EncodeURLPath("src/日本語 #?.cpp")=="src/%E6%97%A5%E6%9C%AC%E8%AA%9E%20%23%3F.cpp");
    CHECK(MapLineToBase("@@ -1,0 +2,2 @@\n+x\n+y\n",1)==1);
    CHECK(!MapLineToBase("@@ -1,0 +2,2 @@\n+x\n+y\n",2));
    CHECK(MapLineToBase("@@ -1,0 +2,2 @@\n+x\n+y\n",4)==2);
    CHECK(MapLineToBase("@@ -2,2 +1,0 @@\n-x\n-y\n",1)==1);
    CHECK(MapLineToBase("@@ -2,2 +1,0 @@\n-x\n-y\n",2)==4);
    CHECK(!MapLineToBase("@@ -2 +2 @@\n-x\n+y\n",2));
    CommitGraph graph;
    Commit merge;merge.hash="m";merge.parents={"a","b"};auto row=graph.Append(merge);
    CHECK(row.lane==0);CHECK(row.edges.size()==2);CHECK(row.width==2);
    Commit a;a.hash="a";a.parents={"root"};row=graph.Append(a);CHECK(row.lane==0);CHECK(row.width==2);
    Commit b;b.hash="b";b.parents={"root"};row=graph.Append(b);CHECK(row.lane==1);CHECK(row.edges.size()==2);
    Commit root;root.hash="root";row=graph.Append(root);CHECK(row.width==1);CHECK(row.edges.empty());
}
void Recovery() {
    Temporary t;auto file=(fs::path(t.path)/"test.draft").string();
    Draft draft;draft.path="/a path/日本.cpp";draft.name="日本.cpp";draft.text="first\r\n日本語 😀\r\n";
    draft.base={3,5,19,1234,5678,true};draft.caret=10;draft.anchor=3;draft.firstLine=1;draft.bom=true;draft.eol=0;
    CHECK(WriteDraft(file,draft).empty());CHECK(ListDrafts(t.path).size()==1);
    auto copy=ReadDraft(file);CHECK(copy.ok());CHECK(copy.text==draft.text);CHECK(copy.path==draft.path);CHECK(copy.base==draft.base);CHECK(copy.bom);CHECK(copy.eol==0);CHECK(copy.caret==10);CHECK(copy.anchor==3);
    std::string text;copy=ReadDraft(file,[&](auto chunk){text+=chunk;return true;});CHECK(copy.ok());CHECK(text==draft.text);CHECK(copy.text.empty());
    auto contents=ReadFile(file); // Binary read retains this small complete record.
    contents.bytes.back()^=1;Put(file,contents.bytes);CHECK(!ReadDraft(file).ok());
    Put(file,contents.bytes.substr(0,20));CHECK(!ReadDraft(file).ok());
    CHECK(WriteDraft(file,draft).empty());struct stat mode{};stat(file.c_str(),&mode);CHECK((mode.st_mode&0777)==0600);
}
void GitIntegration() {
    Temporary t;GitRepository git(t.path);
    CHECK(git.Run({"init","-b","main"}).ok());
    CHECK(git.Run({"config","user.email","test@kiri.invalid"}).ok());
    CHECK(git.Run({"config","user.name","Kiri Test"}).ok());
    CHECK(git.Run({"remote","add","origin","git@github.com:example/kiri.git"}).ok());
    std::string file="name with 'quotes' 日本.cpp";
    Put(fs::path(t.path)/file,"one\ntwo\nthree\nfour\n");
    CHECK(git.Stage({file}).ok());CHECK(git.Unstage({file}).ok());
    auto files=ParseGitStatus(git.Status().output);CHECK(files.size()==1);CHECK(files[0].index=='?');
    CHECK(git.Stage({file}).ok());CHECK(git.CommitIndex("Initial commit\n\nA longer description.").ok());
    auto history=ParseGitLog(git.History(0,10).output);CHECK(history.size()==1);CHECK(history[0].subject=="Initial commit");CHECK(history[0].parents.empty());
    auto link=git.Permalink(file,2,3);CHECK(link.ok());CHECK(link.output.find("/blob/"+history[0].hash+"/")!=std::string::npos);CHECK(link.output.find("#L2-L3")!=std::string::npos);
    CHECK(!git.Permalink(file,5,5).ok());CHECK(!git.Permalink(file,1,1000000000).ok());
    Put(fs::path(t.path)/file,"one\nnew\ntwo\nthree\nfour\n");
    link=git.Permalink(file,3,4);CHECK(link.ok());CHECK(link.output.find("#L2-L3")!=std::string::npos);
    CHECK(!git.Permalink(file,2,2).ok());CHECK(!git.Permalink(file,1,3).ok());
    CHECK(git.Diff(file,false).output.find("+new")!=std::string::npos);
    CHECK(git.Stage({file}).ok());CHECK(git.Diff(file,true).output.find("+new")!=std::string::npos);
    CHECK(git.Unstage({file}).ok());CHECK(git.Diff(file,true).output.empty());
    CHECK(git.Stage({file}).ok());CHECK(git.CommitIndex("Second").ok());
    history=ParseGitLog(git.History(0,1).output);CHECK(history.size()==1);CHECK(history[0].subject=="Second");CHECK(history[0].parents.size()==1);
    auto next=ParseGitLog(git.History(1,1).output);CHECK(next.size()==1);CHECK(next[0].hash==history[0].parents[0]);
    CHECK(git.CommitDiff(history[0].hash).output.find("+new")!=std::string::npos);
    CHECK(git.Run({"mv",file,"renamed\nwith newline.cpp"}).ok());
    files=ParseGitStatus(git.Status().output);CHECK(files.size()==1);CHECK(files[0].path=="renamed\nwith newline.cpp");CHECK(files[0].originalPath==file);
    // Unstaging a rename restores both names in the index and keeps the
    // renamed file; other staged changes stay staged (#9).
    CHECK(git.Run({"commit","-qm","Rename"}).ok());CHECK(git.Run({"mv","renamed\nwith newline.cpp","back.cpp"}).ok());
    Put(fs::path(t.path)/"other.txt","other\n");CHECK(git.Stage({"other.txt"}).ok());
    files=ParseGitStatus(git.Status().output);auto rename=std::find_if(files.begin(),files.end(),[](auto& f){return f.index=='R';});CHECK(rename!=files.end());
    CHECK((UnstagePaths({*rename})==std::vector<std::string>{"back.cpp","renamed\nwith newline.cpp"}));
    CHECK(git.Unstage(UnstagePaths({*rename})).ok());
    CHECK(git.Run({"diff","--cached","--name-only","-z"}).output==std::string("other.txt")+'\0');
    CHECK(fs::exists(fs::path(t.path)/"back.cpp"));CHECK(!fs::exists(fs::path(t.path)/"renamed\nwith newline.cpp"));
    fs::rename(fs::path(t.path)/"back.cpp",fs::path(t.path)/"renamed\nwith newline.cpp");CHECK(git.Unstage({"other.txt"}).ok());fs::remove(fs::path(t.path)/"other.txt");
    CHECK(ParseGitStatus(git.Status().output).empty());
    Put(fs::path(t.path)/".gitignore","ignored/\n");fs::create_directory(fs::path(t.path)/"ignored");Put(fs::path(t.path)/"ignored/secret","dont index");
    auto index=IndexProject(t.path);CHECK(!index.paths.empty());
    for(auto& p:index.paths) CHECK(p.find("ignored/")!=0);
    auto found=QuickOpen(index,"rwn");CHECK(!found.empty());
    auto search=SearchProject(t.path,index,"three",true);CHECK(search.matches.size()==1);CHECK(search.matches[0].line==4);
}
// Open Quickly folds non-ASCII case too, still preferring the exact case (#37).
void QuickOpenCase() {
    CHECK(FuzzyScore("école","École.txt")>-1000000);CHECK(FuzzyScore("ÉCOLE","école.txt")>-1000000);
    CHECK(FuzzyScore("école","école.txt")>FuzzyScore("école","École.txt"));
    CHECK(FuzzyScore("αθήνα","Αθήνα.md")>-1000000);CHECK(FuzzyScore("ΑΘΉΝΑ","αθήνα.md")>-1000000);CHECK(FuzzyScore("σ","Σ.md")>-1000000);
    CHECK(FuzzyScore("ПРИВЕТ","привет.txt")>-1000000);CHECK(FuzzyScore("żółw","ŻÓŁW.txt")>-1000000);CHECK(FuzzyScore("ẞ","ß")<=-1000000);
    CHECK(FuzzyScore("e","é")<=-1000000);CHECK(FuzzyScore("rdme","README.md")>-1000000);
    ProjectIndex index;index.paths={"docs/École.txt","src/ecole.cpp","Ελλάδα/Αθήνα.md"};
    auto found=QuickOpen(index,"école");CHECK(found.size()==1 && found[0]=="docs/École.txt");
    found=QuickOpen(index,"αθ");CHECK(found.size()==1 && found[0]=="Ελλάδα/Αθήνα.md");
    // The common ASCII path index stays fast.
    ProjectIndex large;for(int i=0;i<200000;++i) large.paths.push_back("src/module"+std::to_string(i%97)+"/source_file_"+std::to_string(i)+".cpp");
    auto began=std::chrono::steady_clock::now();found=QuickOpen(large,"m42sf999");CHECK(!found.empty());
    CHECK(std::chrono::steady_clock::now()-began<std::chrono::seconds(2));
}
void Terminal() {
    TerminalModel model(3,20,5);
    std::string data="hello\r\n\x1b[31mred\x1b[0m";model.Feed(data.data(),data.size());
    CHECK(model.Text(0,1)=="hello\nred");auto red=model.Cell(1,0);CHECK(red.chars[0]=='r');CHECK(VTERM_COLOR_IS_INDEXED(&red.fg));
    model.Colors(0x123456,0xfedcba);auto defaults=model.Cell(0,0);CHECK(defaults.fg.rgb.red==0x12 && defaults.fg.rgb.green==0x34 && defaults.bg.rgb.blue==0xba);
    std::array<uint32_t,16> palette{};palette[1]=0xabcdef;model.Palette(palette);red=model.Cell(1,0);vterm_screen_convert_color_to_rgb(model.Screen(),&red.fg);CHECK(red.fg.rgb.red==0xab && red.fg.rgb.blue==0xef);
    data="\x1b[?1049h\x1b[HALT";model.Feed(data.data(),data.size());CHECK(model.AlternateScreen());CHECK(model.Text(0,0)=="ALT");
    data="\x1b[?1049l";model.Feed(data.data(),data.size());CHECK(!model.AlternateScreen());CHECK(model.Text(0,0)=="hello");
    for(int n=0;n<12;++n) { data="\r\nline "+std::to_string(n);model.Feed(data.data(),data.size()); }
    CHECK(model.ScrollbackSize()==5);
    model.Colors(0x654321,0x102030);auto history=model.Cell(0,0,3);CHECK(history.fg.rgb.red==0x65 && history.bg.rgb.blue==0x30);
    model.Resize(4,30);CHECK(model.Rows()==4);CHECK(model.Columns()==30);
    model.Character('c',VTERM_MOD_CTRL);CHECK(model.TakeOutput()==std::string(1,3));
    data="\x1b[?2004h";model.Feed(data.data(),data.size());model.Paste("one\ntwo");CHECK(model.TakeOutput()=="\x1b[200~one\ntwo\x1b[201~");
    model.Reset();CHECK(model.ScrollbackSize()==0);CHECK(model.Text(0,0).empty());CHECK(!model.AlternateScreen());
    PtySession session;Temporary t;CHECK(session.Start(t.path,24,80).empty());
    std::string command="unset PROMPT_COMMAND; PS1='KIRI_'READY'>'; printf 'KIRI_PTY_%s\\n' OK\n";CHECK(session.Write(command.data(),command.size()));
    std::string output;auto start=std::chrono::steady_clock::now();char buffer[4096];
    while(std::chrono::steady_clock::now()-start<std::chrono::seconds(5)) {
        auto n=session.Read(buffer,sizeof(buffer));if(n>0) output.append(buffer,n);
        if(output.find("KIRI_PTY_OK")!=std::string::npos && output.find("KIRI_READY>")!=std::string::npos) break;
    }
    CHECK(output.find("KIRI_PTY_OK")!=std::string::npos);CHECK(session.Running());
    CHECK(output.find("KIRI_READY>")!=std::string::npos);
    // A large bracketed paste to a program that is not reading yet arrives
    // complete and in order: unsent bytes are kept, not dropped (#15).
    auto received=t.path+"/received";
    command="stty raw -echo; printf 'KIRI_%s\\n' RAW; sleep 1; head -c 300000 > '"+received+"'; stty sane; printf 'KIRI_%s\\n' DONE\n";
    CHECK(session.Write(command.data(),command.size()));output.clear();start=std::chrono::steady_clock::now();
    while(output.find("KIRI_RAW")==std::string::npos && std::chrono::steady_clock::now()-start<std::chrono::seconds(5)) { auto n=session.Read(buffer,sizeof(buffer));if(n>0) output.append(buffer,n); }
    CHECK(output.find("KIRI_RAW")!=std::string::npos);
    std::string paste="\x1b[200~";for(int i=0;paste.size()<300000-6;++i) paste+=char('a'+i%26);paste+="\x1b[201~";CHECK(paste.size()==300000);
    std::string pending=paste;size_t partial=0;start=std::chrono::steady_clock::now();
    while(!pending.empty() && std::chrono::steady_clock::now()-start<std::chrono::seconds(20)) {
        CHECK(session.Flush(pending,10));if(!pending.empty()) ++partial; // the PTY was full: the rest waits
        auto n=session.Read(buffer,sizeof(buffer),5);if(n>0) output.append(buffer,n);
    }
    CHECK(pending.empty());CHECK(partial>0);
    while(output.find("KIRI_DONE")==std::string::npos && std::chrono::steady_clock::now()-start<std::chrono::seconds(20)) { auto n=session.Read(buffer,sizeof(buffer));if(n>0) output.append(buffer,n); }
    CHECK(output.find("KIRI_DONE")!=std::string::npos);CHECK(ReadFile(received).bytes==paste);
    CHECK(session.Write("\x04",1));start=std::chrono::steady_clock::now();
    while(session.Running() && std::chrono::steady_clock::now()-start<std::chrono::seconds(3)) {
        session.Read(buffer,sizeof(buffer));
    }
    CHECK(!session.Running());session.Stop();CHECK(!session.Running());
}
void TerminalIsolation() {
    Temporary firstDirectory,secondDirectory;PtySession first,second;
    CHECK(first.Start(firstDirectory.path,24,80).empty());
    CHECK(second.Start(secondDirectory.path,24,80).empty());
    auto exchange=[](PtySession& session,const std::string& command,const std::string& expected) {
        if(!session.Write(command.data(),command.size())) return false;
        std::string output;char buffer[4096];auto start=std::chrono::steady_clock::now();
        while(std::chrono::steady_clock::now()-start<std::chrono::seconds(5)) {
            auto count=session.Read(buffer,sizeof(buffer));if(count<0) return false;
            if(count>0) output.append(buffer,count);
            if(output.find(expected)!=std::string::npos) return true;
        }
        std::cerr<<"Expected terminal output: "<<expected<<"\nReceived: "<<output<<'\n';
        return false;
    };
    CHECK(exchange(first,"KIRI_TAB=first; printf 'KIRI_%s:%s:%s\\n' READY \"$KIRI_TAB\" \"$PWD\"\n","KIRI_READY:first:"+CanonicalPath(firstDirectory.path)));
    CHECK(exchange(second,"KIRI_TAB=second; printf 'KIRI_%s:%s:%s\\n' READY \"$KIRI_TAB\" \"$PWD\"\n","KIRI_READY:second:"+CanonicalPath(secondDirectory.path)));
    first.Stop();CHECK(!first.Running());CHECK(second.Running());
    CHECK(exchange(second,"printf 'KIRI_%s:%s\\n' SURVIVED \"$KIRI_TAB\"\n","KIRI_SURVIVED:second"));
    second.Stop();CHECK(!second.Running());
}
int main() {
    signal(SIGPIPE,SIG_IGN);
    try { Processes();Files();InPlaceSaveFailures();SpecialFiles();LinksAndGraph();GitIntegration();QuickOpenCase();Terminal();TerminalIsolation();Recovery(); }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
    std::cout<<"Passed "<<checks<<" checks: processes, safe saves, Git, project search and real PTY terminal.\n";
}
