#pragma once
#include <OutlineListView.h>
#include "core/Project.h"
#include "ui/Theme.h"
#include "ui/FileIcons.h"
#include <functional>
#include <set>
namespace kiri {
class FileItem:public BListItem {
public:
    FileItem(DirectoryEntry value,uint32 level=0,bool placeholder=false);
    void DrawItem(BView* owner,BRect rect,bool complete=false) override;
    void Update(BView* owner,const BFont* font) override;
    DirectoryEntry entry;
    bool loaded=false,placeholder=false;
private:
    std::shared_ptr<const BBitmap> fIcon;
};
class Explorer:public BOutlineListView {
public:
    Explorer();
    ~Explorer() override;
    void MouseDown(BPoint where) override;
    void KeyDown(const char* bytes,int32 count) override;
    void MessageReceived(BMessage* message) override;
    void ApplyTheme(const Theme& theme) { fTheme=theme;SetViewColor(theme.panel);SetLowColor(theme.panel);Invalidate(); }
    const Theme& Colors() const { return fTheme; }
    void Clear();
    void SetRoot(const std::string& path,const DirectoryResult& listing);
    // Updates only the listed folder's rows: unchanged rows (and their
    // expansion and loaded descendants) are kept, removed ones deleted and
    // new ones inserted in place. Selection and scroll position stay.
    void AddListing(const std::string& parent,const DirectoryResult& listing);
    void CheckExpanded();
    // Expands the ancestors of a file inside the root (listing them first if
    // needed), selects its row and scrolls to it. False outside the root.
    bool Reveal(const std::string& path);
    std::vector<std::string> LoadedDirectories() const;
    std::function<void(std::string)> requestDirectory;
private:
    void OpenSelected(bool preview=false);
    void AddEntries(const DirectoryResult& listing,FileItem* parent);
    void UpdateChildren(FileItem* parent,const DirectoryResult& listing);
    void RemoveBranch(FileItem* item);
    FileItem* FindItem(const std::string& path,bool directory) const;
    void CheckExpanded(FileItem* item);
    void ContinueReveal();
    void ClearItems();
    std::string fReveal; // a file being revealed while its folders are listed
    std::string fRoot;
    std::map<std::string,DirectoryResult> fListings;
    std::set<std::string> fExpanded;
    Theme fTheme=Theme::Builtins()[0];
};
}
