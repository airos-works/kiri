#include "ui/Explorer.h"
#include "ui/Messages.h"
#include <Font.h>
#include <Message.h>
#include <String.h>
#include <Window.h>
#include <algorithm>
#include <map>
#include <set>
namespace kiri {
FileItem::FileItem(DirectoryEntry value,uint32 level,bool isPlaceholder)
    :BListItem(level,false),entry(std::move(value)),placeholder(isPlaceholder) {
    if(!placeholder) fIcon=FileIcon(entry.path,entry.directory);
}
void FileItem::Update(BView* owner,const BFont* font) { BListItem::Update(owner,font);SetHeight(25); }
void FileItem::DrawItem(BView* owner,BRect rect,bool) {
    const auto& t=static_cast<Explorer*>(owner)->Colors();
    owner->SetHighColor(IsSelected()?t.selection:t.panel);owner->FillRect(rect);
    owner->SetLowColor(IsSelected()?t.selection:t.panel);
    float x=rect.left+5,y=rect.top+6;
    owner->SetHighColor(entry.directory?t.accent:t.muted);
    if(fIcon) {
        owner->PushState();owner->SetDrawingMode(B_OP_ALPHA);owner->SetBlendingMode(B_PIXEL_ALPHA,B_ALPHA_OVERLAY);
        owner->DrawBitmap(fIcon.get(),BPoint(x,rect.top+4));owner->PopState();
    }
    else if(entry.directory) { owner->FillRoundRect(BRect(x,y+3,x+13,y+12),2,2);owner->FillRect(BRect(x+1,y,x+6,y+4)); }
    else if(!placeholder) { owner->StrokeRect(BRect(x+2,y,x+11,y+13));owner->StrokeLine(BPoint(x+4,y+5),BPoint(x+9,y+5)); }
    owner->SetHighColor(IsSelected()?t.selectionText:placeholder?t.muted:t.text);
    BString label(entry.name.c_str());owner->TruncateString(&label,B_TRUNCATE_MIDDLE,rect.Width()-29);
    owner->DrawString(label.String(),BPoint(x+22,rect.top+18));
}
Explorer::Explorer():BOutlineListView("files",B_SINGLE_SELECTION_LIST,B_WILL_DRAW|B_NAVIGABLE|B_FRAME_EVENTS) {
    SetExplicitMinSize(BSize(150,100));SetInvocationMessage(new BMessage(kOpenFile));
}
Explorer::~Explorer() { Clear(); }
void Explorer::Clear() {
    ClearItems();fRoot.clear();fListings.clear();fExpanded.clear();fReveal.clear();
}
void Explorer::ClearItems() {
    std::vector<BListItem*> items;
    for(int32 i=0;i<FullListCountItems();++i) items.push_back(FullListItemAt(i));
    MakeEmpty();for(auto* item:items) delete item;
}
void Explorer::AddEntries(const DirectoryResult& listing,FileItem* parent) {
    for(size_t i=0;i<listing.entries.size();++i) {
        // AddUnder inserts immediately after the parent; reverse the input to
        // keep the same directories-first ordering as the project root.
        const auto& entry=listing.entries[parent?listing.entries.size()-1-i:i];
        auto* item=new FileItem(entry,parent?parent->OutlineLevel()+1:0);
        if(parent) AddUnder(item,parent);else AddItem(item);
        if(entry.directory) {
            auto found=fListings.find(entry.path);
            if(found!=fListings.end()) { item->loaded=true;AddEntries(found->second,item); }
            else AddUnder(new FileItem({"Loading…",entry.path,false,false,false,{}},item->OutlineLevel()+1,true),item);
            if(fExpanded.count(entry.path)) Expand(item);
        }
    }
    if(!listing.error.empty()) {
        auto* error=new FileItem({listing.error,"",false,false,false,{}},parent?parent->OutlineLevel()+1:0,true);
        if(parent) AddUnder(error,parent);else AddItem(error);
    }
}
void Explorer::SetRoot(const std::string& path,const DirectoryResult& listing) { if(fRoot!=path) Clear();fRoot=path;AddListing(path,listing); }
FileItem* Explorer::FindItem(const std::string& path,bool directory) const {
    for(int32 i=0;i<FullListCountItems();++i) {
        auto* item=static_cast<FileItem*>(FullListItemAt(i));
        if(!item->placeholder && item->entry.directory==directory && item->entry.path==path) return item;
    }
    return nullptr;
}
void Explorer::RemoveBranch(FileItem* item) {
    std::vector<FileItem*> branch{item};
    for(int32 i=0,count=CountItemsUnder(item,false);i<count;++i) branch.push_back(static_cast<FileItem*>(ItemUnderAt(item,false,i)));
    for(auto* row:branch) if(row->entry.directory && !row->placeholder && row->IsExpanded()) fExpanded.insert(row->entry.path);
    // Bottom-up, so every removal is of a row without sub-rows: removing a
    // parent would make the list view delete its sub-rows itself.
    for(auto it=branch.rbegin();it!=branch.rend();++it) { RemoveItem(*it);delete *it; }
}
void Explorer::AddListing(const std::string& path,const DirectoryResult& listing) {
    fListings[path]=listing;
    FileItem* parent=nullptr;
    if(path!=fRoot) {
        // A folder not in the tree yet is shown from the cache when its parent is.
        parent=FindItem(path,true);if(!parent) return;parent->loaded=true;
    }
    auto scroll=Bounds().LeftTop();
    UpdateChildren(parent,listing);
    ScrollTo(scroll);
    if(!fReveal.empty()) ContinueReveal();
}
void Explorer::UpdateChildren(FileItem* parent,const DirectoryResult& listing) {
    auto key=[](const DirectoryEntry& entry,bool placeholder){return std::string(placeholder?"#":"")+entry.path+(entry.directory?"/":"")+(placeholder?entry.name:"");};
    std::vector<FileItem*> current;
    if(parent) for(int32 i=0,count=CountItemsUnder(parent,true);i<count;++i) current.push_back(static_cast<FileItem*>(ItemUnderAt(parent,true,i)));
    else for(int32 i=0;i<FullListCountItems();++i) if(FullListItemAt(i)->OutlineLevel()==0) current.push_back(static_cast<FileItem*>(FullListItemAt(i)));
    struct Wanted { std::string key;const DirectoryEntry* entry; };
    std::vector<Wanted> wanted;DirectoryEntry error{listing.error,"",false,false,false,{}};
    for(auto& entry:listing.entries) wanted.push_back({key(entry,false),&entry});
    if(!listing.error.empty()) wanted.push_back({key(error,true),&error});
    std::map<std::string,FileItem*> existing;for(auto* item:current) existing[key(item->entry,item->placeholder)]=item;
    // Kept rows must already be in the wanted order; otherwise rebuild this folder only.
    std::vector<FileItem*> kept;for(auto& want:wanted) if(auto found=existing.find(want.key);found!=existing.end()) kept.push_back(found->second);
    std::vector<FileItem*> order;for(auto* item:current) if(std::find(kept.begin(),kept.end(),item)!=kept.end()) order.push_back(item);
    if(order!=kept) { for(auto* item:current) RemoveBranch(item);AddEntries(listing,parent);return; }
    std::set<std::string> wantedKeys;for(auto& want:wanted) wantedKeys.insert(want.key);
    for(auto* item:current) if(!wantedKeys.count(key(item->entry,item->placeholder))) RemoveBranch(item);
    int32 position=parent?FullListIndexOf(parent)+1:0;uint32 level=parent?parent->OutlineLevel()+1:0;
    for(auto& want:wanted) {
        FileItem* item=nullptr;
        if(auto found=existing.find(want.key);found!=existing.end()) {
            item=found->second;
            if(!item->placeholder) { item->entry=*want.entry;if(auto index=IndexOf(item);index>=0) InvalidateItem(index); }
        } else {
            bool placeholder=want.entry==&error;item=new FileItem(*want.entry,level,placeholder);AddItem(item,position);
            if(want.entry->directory) {
                auto cached=fListings.find(want.entry->path);
                if(cached!=fListings.end()) { item->loaded=true;AddEntries(cached->second,item); }
                else AddUnder(new FileItem({"Loading…",want.entry->path,false,false,false,{}},level+1,true),item);
                if(fExpanded.count(want.entry->path)) Expand(item);
            }
        }
        position=FullListIndexOf(item)+1+CountItemsUnder(item,false);
    }
}
std::vector<std::string> Explorer::LoadedDirectories() const { std::vector<std::string> result;for(auto& listing:fListings) result.push_back(listing.first);return result; }
void Explorer::CheckExpanded(FileItem* item) {
    if(item && item->entry.directory && !item->placeholder && !item->loaded && item->IsExpanded()) {
        item->loaded=true;if(requestDirectory) requestDirectory(item->entry.path);
    }
}
void Explorer::CheckExpanded() {
    for(int32 i=0;i<FullListCountItems();++i) CheckExpanded(static_cast<FileItem*>(FullListItemAt(i)));
}
bool Explorer::Reveal(const std::string& path) {
    if(fRoot.empty() || path.size()<=fRoot.size() || path.compare(0,fRoot.size(),fRoot)!=0 || (fRoot.back()!='/' && path[fRoot.size()]!='/')) return false;
    fReveal=path;ContinueReveal();return true;
}
void Explorer::ContinueReveal() {
    // Expand each folder on the path to the file, listing it first if needed.
    auto path=fReveal;size_t at=fRoot.size()+(fRoot.back()=='/'?0:1);
    while(true) {
        auto slash=path.find('/',at);if(slash==std::string::npos) break;
        auto folder=path.substr(0,slash);at=slash+1;
        auto* item=FindItem(folder,true);if(!item) return; // its parent is still being listed
        if(!item->IsExpanded()) Expand(item);
        if(!fListings.count(folder)) { if(!item->loaded) { item->loaded=true;if(requestDirectory) requestDirectory(folder); }return; }
    }
    fReveal.clear();
    if(auto* file=FindItem(path,false)) { auto index=IndexOf(file);if(index>=0) { Select(index);ScrollToSelection(); } }
}
void Explorer::OpenSelected(bool preview) {
    auto* item=static_cast<FileItem*>(ItemAt(CurrentSelection()));
    if(!item || item->placeholder) return;
    if(item->entry.directory) { if(!preview) { if(item->IsExpanded()) Collapse(item);else Expand(item);CheckExpanded(item); } }
    else { BMessage msg(kOpenFile);msg.AddString("path",item->entry.path.c_str());msg.AddBool("preview",preview);Window()->PostMessage(&msg); }
}
void Explorer::MouseDown(BPoint where) {
    // Only the row under the mouse can have been expanded by this click.
    BOutlineListView::MouseDown(where);CheckExpanded(static_cast<FileItem*>(ItemAt(IndexOf(where))));
    int32 clicks=1;Window()->CurrentMessage()->FindInt32("clicks",&clicks);
    int32 buttons=B_PRIMARY_MOUSE_BUTTON;Window()->CurrentMessage()->FindInt32("buttons",&buttons);
    if(buttons&B_PRIMARY_MOUSE_BUTTON) OpenSelected(clicks!=2);
}
void Explorer::KeyDown(const char* bytes,int32 count) {
    if(count==1 && (bytes[0]==B_ENTER || bytes[0]==' ')) OpenSelected();
    else { BOutlineListView::KeyDown(bytes,count);CheckExpanded(static_cast<FileItem*>(ItemAt(CurrentSelection())));if(count==1 && (bytes[0]==B_UP_ARROW || bytes[0]==B_DOWN_ARROW)) OpenSelected(true); }
}
void Explorer::MessageReceived(BMessage* message) { BOutlineListView::MessageReceived(message); }
}
