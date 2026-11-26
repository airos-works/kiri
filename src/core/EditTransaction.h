#pragma once
#include "core/Project.h"
#include <functional>

namespace kiri {
struct EditSnapshot {
    std::string path,text;
    FileStamp stamp;
    bool bom=false,open=false,readOnly=false;
    int64_t document=0,revision=0;
    int version=0;
    std::string owner;
};
struct SelectedEdit { TextEdit edit;bool selected=true; };
struct FileEdit {
    EditSnapshot before;
    std::vector<SelectedEdit> edits;
    std::string after,status="Ready",error;
    FileStamp writtenStamp;
    int64_t appliedRevision=0;
    bool applied=false,restored=false,pending=false,checkVersion=false;
    std::vector<TextEdit> Selected() const;
    void Prepare();
};
struct EditPlan {
    std::string title,root,summary,journal;
    bool rename=false;
    std::vector<FileEdit> files;
};
using SnapshotMap=std::map<std::string,EditSnapshot>; // canonical absolute paths
EditSnapshot DiskSnapshot(const std::string& path,const std::atomic<bool>* cancel=nullptr);
EditPlan ReplacementPlan(const std::string& root,const ProjectIndex& index,const ProjectSearchOptions& options,
    const std::string& replacement,const SnapshotMap& open,const std::atomic<bool>* cancel=nullptr);
// Before/after text for reviewing one edit: its line (ended by CR, LF or
// CRLF), cut to about context bytes on each side of the edit at UTF-8
// boundaries, so an edit deep in a long minified line stays visible. Omitted
// text is marked with "…"; very long removed or inserted text is shortened
// in the middle.
struct EditExcerpt { std::string before,after; };
EditExcerpt ExcerptEdit(std::string_view text,const TextEdit& edit,size_t context=2000);
std::string PathFromURI(const std::string& uri);
std::string ValidateRenameName(const std::string& name,const std::string& language={});
// Every URI and range is validated, including operations the client cannot apply.
// The resolver must return the snapshot sent to the server or a checked disk snapshot.
EditPlan WorkspaceEditPlan(const Json& edit,PositionEncoding encoding,
    const std::function<EditSnapshot(const std::string&)>& snapshot);
std::string ValidateEdit(const FileEdit& file,const EditSnapshot& current,bool restore=false);
std::string ApplyDiskEdit(FileEdit& file,bool restore=false);
// The recovery journal is written once: every file's before/after text and
// initial state. Each later change of one file's state is a small line that
// AppendEditJournal adds (and flushes) to plan.journal+".log", so the bytes
// written grow with the files actually changed, not with the whole plan
// per step. ReadEditJournal replays those lines over the snapshot.
std::string WriteEditJournal(const EditPlan& plan);
std::string AppendEditJournal(const EditPlan& plan,size_t file);
EditPlan ReadEditJournal(const std::string& path);
}
