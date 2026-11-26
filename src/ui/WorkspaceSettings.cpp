#include "ui/Workspace.h"
#include "ui/DocumentSettingsWindow.h"
#include "ui/Editor.h"
#include "ui/Messages.h"
#include <StringView.h>
#include <map>

namespace kiri {
void Workspace::ShowDocumentSettings() {
    auto* d=Current();if(!d || !d->editor || d->editor->SendMessage(SCI_GETREADONLY)) {Notice("Open an editable text document first.");return;}
    if(d->settingsWindow.IsValid()) {d->settingsWindow.SendMessage(kActivateWorkspace);return;}
    auto* window=new DocumentSettingsWindow(BMessenger(this),fEditorSettings,d->editor->EffectiveStyle(),d->editor->State()->overrides,Frame(),d->id,d->name);
    d->settingsWindow=BMessenger(window);window->Show();
}
void Workspace::UpdateDocumentStyle(Document& d) {
    for(auto& pane:fPanes) for(auto& tab:pane->tabs) if(tab->document==d.id && tab->editor) tab->editor->ApplyDocumentStyle();
    UpdateStatus();
}
void Workspace::ApplyDocumentSettings(BMessage& message) {
    int64 id=0;if(message.FindInt64("document",&id)!=B_OK) return;auto* d=ByID(id);if(!d || !d->editor) return;
    d->editor->State()->overrides=ReadDocumentOverrides(message);UpdateDocumentStyle(*d);SaveSettings();
}
void Workspace::RefreshDocumentConfigs() {
    // One job per poll for every open document: each .editorconfig candidate
    // is checked once however many tabs share its folder, and only documents
    // with a changed candidate are resolved again.
    if(fConfigBatchPending) return;
    struct Entry { int64 id,serial;std::string path;std::map<std::string,FileStamp> stamps; };
    std::vector<Entry> entries;
    for(auto& d:fDocuments) {
        if(!d->editor || d->path.empty() || d->editor->SendMessage(SCI_GETREADONLY) || d->configPending) continue;
        d->configPending=true;entries.push_back({d->id,++d->configSerial,d->path,d->editor->State()->config.stamps});
    }
    if(entries.empty()) return;
    fConfigBatchPending=true;
    auto release=[this](const std::vector<Entry>& entries) {
        fConfigBatchPending=false;
        for(auto& entry:entries) if(auto* d=ByID(entry.id);d && d->configSerial==entry.serial) d->configPending=false;
    };
    fJobs->Submit([this,entries,release](const auto& cancel) {
        std::map<std::string,FileStamp> current;
        std::vector<std::pair<size_t,std::shared_ptr<DocumentConfig>>> changed;
        for(size_t i=0;i<entries.size() && !cancel;++i) {
            bool stale=entries[i].stamps.empty();
            for(auto& [file,stamp]:entries[i].stamps) {
                auto found=current.find(file);if(found==current.end()) found=current.emplace(file,StatFile(file)).first;
                if(found->second!=stamp) { stale=true;break; }
            }
            if(stale && !cancel) changed.emplace_back(i,std::make_shared<DocumentConfig>(ReadDocumentConfig(entries[i].path,&cancel)));
        }
        return AsyncQueue::Callback([this,entries,release,changed=std::move(changed)] {
            for(auto& [i,config]:changed) {
                auto* d=ByID(entries[i].id);
                if(d && d->path==entries[i].path && d->configSerial==entries[i].serial && d->editor) {d->editor->State()->config=*config;UpdateDocumentStyle(*d);}
            }
            release(entries);
        });
    },{},[this,entries,release](const std::string& error){release(entries);Notice("EditorConfig: "+error);});
}
void Workspace::RefreshDocumentConfig(Document& d,bool force) {
    if(!d.editor || d.path.empty() || d.editor->SendMessage(SCI_GETREADONLY) || (d.configPending && !force)) return;
    auto id=d.id,serial=++d.configSerial;auto path=d.path;auto stamps=d.editor->State()->config.stamps;d.configPending=true;
    fJobs->Submit([this,id,serial,path,stamps=std::move(stamps),force](const auto& cancel) {
        bool changed=force || stamps.empty();for(auto& entry:stamps) {if(cancel) return AsyncQueue::Callback{};if(StatFile(entry.first)!=entry.second) changed=true;}
        std::shared_ptr<DocumentConfig> config;if(changed && !cancel) config=std::make_shared<DocumentConfig>(ReadDocumentConfig(path,&cancel));
        return AsyncQueue::Callback([this,id,serial,path,config] {auto* d=ByID(id);if(!d || d->path!=path || d->configSerial!=serial) return;d->configPending=false;
            if(config && d->editor) {d->editor->State()->config=*config;UpdateDocumentStyle(*d);}});
    },"document-config-"+std::to_string(id),[this,id,serial](const std::string& error){if(auto* d=ByID(id);d && d->configSerial==serial) {d->configPending=false;Notice("EditorConfig: "+error);}});
}
}
