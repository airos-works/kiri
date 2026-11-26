#pragma once
#include "core/Terminal.h"
#include "ui/Theme.h"
#include <View.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
class BMessageRunner;
namespace kiri {
class TerminalView:public BView {
public:
    TerminalView();
    ~TerminalView() override;
    void AttachedToWindow() override;
    void FrameResized(float width,float height) override;
    void Draw(BRect update) override;
    void KeyDown(const char* bytes,int32 count) override;
    void MouseDown(BPoint where) override;
    void MouseMoved(BPoint where,uint32 transit,const BMessage* drag) override;
    void MouseUp(BPoint where) override;
    void MessageReceived(BMessage* message) override;
    void MakeFocus(bool focus=true) override;
    void Start(const std::string& directory);
    void ApplyTheme(const Theme& theme);
    bool Exited() const { return fExited.load(); }
private:
    friend struct WorkspaceTestAccess;
    void Stop();
    void FlushInput();
    void Tick();
    void Copy();
    void Paste();
    // A cell boundary (column 0..Columns()) on an absolute content line.
    struct CellPosition { int64_t line=0;int column=0; };
    CellPosition PositionAt(BPoint where,bool boundary) const;
    void UpdateSelection();
    bool Selected(int64_t line,int column) const;
    TerminalModel fModel;
    PtySession fSession;
    std::unique_ptr<BMessageRunner> fTimer;
    std::thread fWorker;
    std::atomic<bool> fStop{false},fExited{false};
    std::mutex fMutex;
    std::string fIncoming,fOutgoing;
    std::string fDirectory;
    float fCellWidth=8,fCellHeight=18,fAscent=13;
    int fScrollOffset=0;
    // Drag from fAnchor to fCaret; fSelectionStart/End are the normalized
    // cell range [start,end). Mode 1 selects cells, 2 words, 3 whole rows.
    CellPosition fAnchor,fCaret,fSelectionStart,fSelectionEnd;
    int fSelectionMode=0;
    bool fSelecting=false;
    bool fExitReported=false;
    Theme fTheme=Theme::Builtins()[0];
};
}
