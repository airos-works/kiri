#pragma once
#include <optional>
#include <ScintillaView.h>
#include "ui/Theme.h"
#include "ui/EditorSettings.h"
#include "core/LanguageProtocol.h"
#include "core/Search.h"
#include "core/LanguageAnalysis.h"
#include <string>
#include <ILoader.h>
#include <memory>
namespace kiri {
class MinimapView;
// Scintilla owns the text and undo stack through each attached view's document
// reference. This state follows that same buffer, including synchronous input
// revisions (Haiku delivers Scintilla's notifications asynchronously).
struct EditorState {
    int64 revision=0,inputRevision=0,changes=0;bool recovered=false;
    std::vector<Diagnostic> diagnostics;
    std::vector<SemanticToken> semanticTokens;
    DocumentConfig config;
    DocumentOverrides overrides;
};
struct EditorLoader {
    Scintilla::ILoader* loader=nullptr;
    ~EditorLoader() { if(loader) loader->Release(); }
    void* Take() { auto* document=loader->ConvertToDocument();loader=nullptr;return document; }
};
class Editor : public BScintillaView {
public:
    Editor();
    void AllAttached() override;
    void FrameResized(float width,float height) override;
    void DoLayout() override;
    MinimapView* Minimap() const {return fMinimap;}
    sptr_t SendMessage(unsigned int message,uptr_t wParam=0,sptr_t lParam=0);
    void NotificationReceived(SCNotification* notification) override;
    void ContextMenu(BPoint where) override;
    void ApplyTheme(const Theme& theme);
    void ApplySettings(const EditorSettings& settings);
    void ApplyDocumentStyle();
    DocumentStyle EffectiveStyle() const;
    bool FilterEditingInput(BMessage* message);
    void InsertNewline();
    // reindent=false keeps the clipboard's leading tabs, spaces and
    // whitespace-only lines exactly (Paste Without Reindenting).
    void PasteText(const std::string& text,bool reindent=true);
    void SetLanguage(const std::string& path,bool large=false);
    void SetText(const std::string& bytes,bool readOnly=false,int eol=2);
    std::shared_ptr<EditorLoader> CreateLoader(bool large);
    void Adopt(EditorLoader& loader,int eol=2);
    void ShareDocument(Editor& source);
    std::shared_ptr<EditorState> State() const { return fState; }
    void NoteInput();
    void SetDiagnostics(const std::vector<Diagnostic>& diagnostics);
    // Applies the visible tokens and a bounded slice at once, the rest in
    // bounded slices from this view's message loop; an edit or newer tokens
    // stop work in progress. One indicator change per run of tokens.
    void SetSemanticTokens(const std::vector<SemanticToken>& tokens);
    bool SemanticPending() const { return fSemanticNext<fSemanticFills.size(); }
    bigtime_t SemanticLongestSlice() const { return fSemanticLongestSlice; }
    void MessageReceived(BMessage* message) override;
    void ClearAnalysis();
    void ShowDiagnostic(size_t position);
    int64 InputRevision() const { return fState->inputRevision; }
    std::string Text();
    bool Matches(std::string_view text);
    void ApplyEdits(const std::vector<TextEdit>& edits,bool preserveLines=false);
    void ShowCompletions(const std::vector<std::string>& labels);
    void CancelCompletions();
    bool FilterLanguageKey(BMessage* message);
    bool Dirty() { return fState->recovered || SendMessage(SCI_GETMODIFY)!=0; }
    void MarkRecovered() { fState->recovered=true;++fState->changes;NoteInput(); }
    void MarkSaved() { fState->recovered=false;SendMessage(SCI_SETSAVEPOINT); }
    int64 Revision() const { return fState->revision; }
    const std::string& Language() const { return fLanguage; }
    bool Find(const std::string& query,bool backwards=false,bool matchCase=false,bool regex=false);
    bool ReplaceOne(const std::string& query,const std::string& replacement,bool matchCase=false);
    int ReplaceAll(const std::string& query,const std::string& replacement,bool matchCase=false);
    void SetSearchSelection(bool enabled);
    TextMatches SearchMatches(const SearchOptions& options,bool highlight=true);
    bool Find(const SearchOptions& options,bool backwards=false);
    int Replace(const SearchOptions& options,const std::string& replacement,bool all);
    const std::string& SearchError() const { return fSearchError; }
    void ClearSearchHighlights();
    void GoTo(size_t line,size_t column=1,bool focus=true);
    // Column as the status bar shows it: tabs expanded to the tab width (Go to Line).
    void GoToVisualColumn(size_t line,size_t column,bool focus=true);
    // The clipboard as one paste; false for multiple or rectangular
    // selections, which keep Scintilla's own paste.
    bool PasteClipboard(bool reindent=true);
    // Whether the In Selection scope (if on) still holds; sets SearchError.
    bool ScopeReady();
    std::optional<TextEdit> NextMatch(const TextQuery& query,const std::string& text,bool backwards,const std::string* replacement=nullptr);
private:
    void ApplySemanticSlice();
    struct SemanticFill { int indicator;size_t start,end; };
    std::vector<SemanticFill> fSemanticFills;
    size_t fSemanticNext=0;
    int64 fSemanticGeneration=0;
    bigtime_t fSemanticLongestSlice=0;
    void LayoutMinimap();
    MinimapView* fMinimap=nullptr;
    bool fLayingOutMinimap=false;
    void ReplaceSelections(const std::vector<std::string>& replacements);
    void Style(int id,rgb_color color,bool bold=false);
    void UpdateMarginWidth();
    std::string fLexer="null",fLanguage="Plain Text";
    Theme fTheme=Theme::Builtins()[0];
    EditorSettings fSettings;
    bool fLoading=false;
    int fDigits=0;
    std::shared_ptr<EditorState> fState=std::make_shared<EditorState>();
    std::vector<std::string> fCompletionLabels;
    bool fSelectionSearch=false;
    size_t fScopeStart=0,fScopeEnd=0;
    std::string fScopeText,fSearchError,fLastQuery;
    sptr_t fLastEmpty=-1;
};
}
