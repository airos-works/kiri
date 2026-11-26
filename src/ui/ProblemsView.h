#pragma once
#include "core/LanguageAnalysis.h"
#include "ui/Theme.h"
#include <View.h>
class BListView;class BStringView;class BTextView;class BMenuField;
namespace kiri {
// index: position in the document's diagnostics for that analysis serial,
// which tells apart problems that start at the same offset.
struct ProblemEntry { int64 document=0,serial=0;std::string path;Diagnostic diagnostic;size_t index=0; };
class ProblemsView:public BView {
public:
    ProblemsView();
    ~ProblemsView() override;
    void AttachedToWindow() override;
    void MessageReceived(BMessage* message) override;
    void ApplyTheme(const Theme& theme);
    void SetProblems(std::vector<ProblemEntry> entries,const std::string& status);
    const std::vector<ProblemEntry>& Entries() const { return fEntries; }
    int SeverityFilter() const { return fFilter; }
    void SelectProblem(int64 document,int64 serial,size_t index);
private:
    void Rebuild();
    const ProblemEntry* Selected() const;
    BListView* fList;
    BStringView* fStatus;
    BTextView* fDetails;
    BMenuField* fSeverity;
    std::vector<ProblemEntry> fEntries;
    std::vector<int> fRows;
    int fFilter=0;
    Theme fTheme=Theme::Builtins()[0];
};
}
