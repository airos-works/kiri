#pragma once
#include "core/EditTransaction.h"
#include "ui/Theme.h"
#include <Window.h>
#include <Messenger.h>
class BListView;class BTextView;class BStringView;class BButton;
namespace kiri {
class EditPreviewWindow : public BWindow {
public:
    EditPreviewWindow(BWindow* owner,const EditPlan& plan,const Theme& theme,bool restore,int64 serial);
    ~EditPreviewWindow() override;
    void MessageReceived(BMessage* message) override;
    bool QuitRequested() override;
private:
    friend struct WorkspaceTestAccess;
    // The list is built once; progress and Include / Exclude update only the
    // rows they change, so a large preview stays responsive while applying.
    void BuildList();
    std::string FileLabel(size_t file) const;
    std::string MatchLabel(size_t file,size_t match) const;
    void UpdateRow(int32 row);
    void UpdateApply();
    void Detail();
    void SendAction(uint32 command);
    EditPlan fPlan;
    BMessenger fTarget;
    BListView* fList;
    BTextView* fDetail;
    BStringView* fStatus;
    BButton *fApply,*fToggle,*fRefresh,*fCancel;
    bool fRestore=false,fBusy=false;
    int64 fSerial=0;
    std::vector<std::pair<size_t,int>> fRows;
    std::vector<int32> fFileRows; // list row of each file
    std::vector<std::vector<std::string>> fMatchText; // "line:column  before → after", computed once
};
}
