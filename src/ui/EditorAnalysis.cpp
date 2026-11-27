#include "ui/Editor.h"
#include <algorithm>
#include <array>
#include <Window.h>

namespace kiri {
void Editor::NoteInput() {
    ++fState->revision;++fState->inputRevision;ClearAnalysis();
}
namespace {
constexpr uint32 kSemanticSlice='smsl';
constexpr size_t kSemanticSliceFills=6000; // a few milliseconds of indicator work
}
void Editor::ClearAnalysis() {
    ++fSemanticGeneration;fSemanticFills.clear();fSemanticNext=0;
    for(int indicator=9;indicator<=24;++indicator) {
        if(indicator>=18 && indicator<=20) continue; // Find uses 8; diff ranges use 20.
        SendMessage(SCI_SETINDICATORCURRENT,indicator);SendMessage(SCI_INDICATORCLEARRANGE,0,SendMessage(SCI_GETLENGTH));
    }
    fState->diagnostics.clear();fState->semanticTokens.clear();SendMessage(SCI_CALLTIPCANCEL);
}
void Editor::SetDiagnostics(const std::vector<Diagnostic>& diagnostics) {
    auto length=SendMessage(SCI_GETLENGTH);
    for(int i=21;i<25;++i) {SendMessage(SCI_SETINDICATORCURRENT,i);SendMessage(SCI_INDICATORCLEARRANGE,0,length);}
    fState->diagnostics=diagnostics;
    for(auto& item:diagnostics) {
        auto start=std::min<sptr_t>(item.start,length),end=std::min<sptr_t>(item.end,length);
        if(start==end && length) {if(end<length) end=SendMessage(SCI_POSITIONAFTER,end);else start=SendMessage(SCI_POSITIONBEFORE,start);}
        SendMessage(SCI_SETINDICATORCURRENT,20+std::clamp(item.severity,1,4));SendMessage(SCI_INDICATORFILLRANGE,start,end-start);
    }
}
void Editor::SetSemanticTokens(const std::vector<SemanticToken>& tokens) {
    ++fSemanticGeneration;fSemanticLongestSlice=0;
    for(int i=9;i<=17;++i) {SendMessage(SCI_SETINDICATORCURRENT,i);SendMessage(SCI_INDICATORCLEARRANGE,0,SendMessage(SCI_GETLENGTH));}
    fState->semanticTokens=tokens;
    // Group fills by indicator (merging touching spans), visible lines first.
    std::array<std::vector<SemanticFill>,9> groups;
    auto add=[&](int indicator,size_t start,size_t end) {
        auto& runs=groups[indicator-9];if(!runs.empty() && runs.back().end==start) runs.back().end=end;else runs.push_back({indicator,start,end});
    };
    for(auto& token:tokens) { add(9+int(token.role),token.start,token.end);if(token.deprecated) add(17,token.start,token.end); }
    auto firstLine=SendMessage(SCI_DOCLINEFROMVISIBLE,SendMessage(SCI_GETFIRSTVISIBLELINE));
    size_t visibleStart=SendMessage(SCI_POSITIONFROMLINE,firstLine),visibleEnd=SendMessage(SCI_GETLINEENDPOSITION,firstLine+SendMessage(SCI_LINESONSCREEN)+1);
    fSemanticFills.clear();fSemanticNext=0;
    for(int pass=0;pass<2;++pass) for(auto& runs:groups) for(auto& fill:runs) {
        bool visible=fill.end>=visibleStart && fill.start<=visibleEnd;
        if(visible==(pass==0)) fSemanticFills.push_back(fill);
    }
    ApplySemanticSlice();
}
void Editor::ApplySemanticSlice() {
    auto began=system_time();int current=-1;size_t done=0;
    while(fSemanticNext<fSemanticFills.size() && done<kSemanticSliceFills) {
        auto& fill=fSemanticFills[fSemanticNext++];++done;
        if(fill.indicator!=current) { current=fill.indicator;SendMessage(SCI_SETINDICATORCURRENT,current); }
        SendMessage(SCI_INDICATORFILLRANGE,fill.start,fill.end-fill.start);
    }
    fSemanticLongestSlice=std::max(fSemanticLongestSlice,system_time()-began);
    if(fSemanticNext>=fSemanticFills.size()) { std::vector<SemanticFill>().swap(fSemanticFills);fSemanticNext=0;return; }
    // Let input and painting run between slices; without a window, finish now.
    if(!Window()) { ApplySemanticSlice();return; }
    BMessage slice(kSemanticSlice);slice.AddInt64("generation",fSemanticGeneration);Window()->PostMessage(&slice,this);
}
void Editor::MessageReceived(BMessage* message) {
    if(message->what==kSemanticSlice) { int64 generation=-1;message->FindInt64("generation",&generation);if(generation==fSemanticGeneration && SemanticPending()) ApplySemanticSlice();return; }
    BScintillaView::MessageReceived(message);
}
void Editor::ShowDiagnostic(size_t position) {
    std::string message;
    for(auto& item:fState->diagnostics) if(position>=item.start && position<=item.end) {
        if(!message.empty()) message+="\n\n";
        message+=SeverityName(item.severity);if(!item.source.empty()) message+=" · "+item.source;if(!item.code.empty()) message+=" "+item.code;
        message+="\n"+item.message;if(message.size()>8192) break;
    }
    if(!message.empty()) SendMessage(SCI_CALLTIPSHOW,position,reinterpret_cast<sptr_t>(message.c_str()));
}
}
