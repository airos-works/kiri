#include "core/LanguageProtocol.h"
#include "core/FileIO.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include <stdexcept>

namespace kiri {
namespace {
size_t CharacterBytes(std::string_view text,size_t at) {
    unsigned char c=text[at];size_t length=c<0x80?1:c<0xe0?2:c<0xf0?3:4;
    return std::min(length,text.size()-at);
}
size_t Units(size_t bytes,PositionEncoding encoding) { return encoding==PositionEncoding::UTF8?bytes:encoding==PositionEncoding::UTF16 && bytes==4?2:1; }
TextEdit ReadEdit(const Json& value,std::string_view text,PositionEncoding encoding) {
    const auto& range=value.at("range");
    return {OffsetAt(text,ReadPosition(range.at("start")),encoding),OffsetAt(text,ReadPosition(range.at("end")),encoding),value.at("newText").get<std::string>()};
}
}
TextPosition PositionAt(std::string_view text,size_t offset,PositionEncoding encoding) {
    TextPosition result;offset=std::min(offset,text.size());
    for(size_t at=0;at<offset;) {
        if(text[at]=='\r' || text[at]=='\n') {
            if(text[at]=='\r' && at+1<text.size() && text[at+1]=='\n') ++at;
            ++at;++result.line;result.character=0;
        } else {
            auto bytes=CharacterBytes(text,at);if(at+bytes>offset) break;
            result.character+=Units(bytes,encoding);at+=bytes;
        }
    }
    return result;
}
size_t OffsetAt(std::string_view text,TextPosition position,PositionEncoding encoding) {
    size_t at=0,line=0,character=0;
    while(at<text.size() && line<position.line) {
        if(text[at]=='\r') { ++line;++at;if(at<text.size() && text[at]=='\n') ++at; }
        else if(text[at++]=='\n') ++line;
    }
    while(at<text.size() && text[at]!='\n' && text[at]!='\r' && character<position.character) {
        auto bytes=CharacterBytes(text,at),units=Units(bytes,encoding);
        if(character+units>position.character) break;
        at+=bytes;character+=units;
    }
    return at;
}
PositionMap::PositionMap(std::string_view text,PositionEncoding encoding):fText(text) {
    // The same walk as PositionAt/OffsetAt, so malformed bytes split lines identically.
    fLines.push_back(0);size_t unit=0;
    for(size_t at=0;at<text.size();) {
        if(text[at]=='\r' || text[at]=='\n') {
            if(text[at]=='\r' && at+1<text.size() && text[at+1]=='\n') { ++at;++unit; }
            ++at;++unit;fLines.push_back(at);continue;
        }
        auto bytes=CharacterBytes(text,at),units=Units(bytes,encoding);
        if(bytes!=1 || units!=1) fWide.push_back({at,unit,bytes,units});
        at+=bytes;unit+=units;
    }
}
size_t PositionMap::UnitAt(size_t byte) const {
    // Units before byte, which is a character boundary.
    auto found=std::upper_bound(fWide.begin(),fWide.end(),byte,[](size_t b,const Character& c){return b<c.byte;});
    if(found==fWide.begin()) return byte;
    --found;if(byte<found->byte+found->bytes) return found->unit; // at its first byte
    return found->unit+found->units+(byte-found->byte-found->bytes);
}
size_t PositionMap::Offset(TextPosition position) const {
    if(position.line>=fLines.size()) return fText.size();
    size_t start=fLines[position.line],end=start;
    if(position.line+1<fLines.size()) { end=fLines[position.line+1];if(end>start && fText[end-1]=='\n') --end;if(end>start && fText[end-1]=='\r') --end; }
    else end=fText.size();
    size_t base=UnitAt(start),target=base+std::min(position.character,end-start+fText.size()); // bounded: no overflow
    // The last wide character starting at or before the target unit.
    auto found=std::upper_bound(fWide.begin(),fWide.end(),target,[](size_t u,const Character& c){return u<c.unit;});
    size_t byte;
    if(found==fWide.begin() || (found-1)->byte<start) byte=start+(target-base);
    else {
        auto& c=*(found-1);
        if(target<c.unit+c.units) byte=c.byte; // inside the character: stop before it
        else byte=c.byte+c.bytes+(target-c.unit-c.units);
    }
    return std::min(byte,end);
}
TextPosition PositionMap::Position(size_t offset) const {
    offset=std::min(offset,fText.size());
    auto line=size_t(std::upper_bound(fLines.begin(),fLines.end(),offset)-fLines.begin())-1;
    // Between the CR and LF of a CRLF pair, PositionAt has already moved on.
    if(offset>0 && offset<fText.size() && fText[offset-1]=='\r' && fText[offset]=='\n' && fLines[line]!=offset) {
        auto owner=std::upper_bound(fWide.begin(),fWide.end(),offset-1,[](size_t b,const Character& c){return b<c.byte;});
        bool swallowed=owner!=fWide.begin() && (owner-1)->byte+(owner-1)->bytes>offset-1; // by a malformed lead byte
        if(!swallowed) return {line+1,0};
    }
    // An offset inside a character counts only the characters before it.
    auto found=std::upper_bound(fWide.begin(),fWide.end(),offset,[](size_t b,const Character& c){return b<c.byte;});
    if(found!=fWide.begin() && (found-1)->byte<offset && offset<(found-1)->byte+(found-1)->bytes && (found-1)->byte>=fLines[line]) offset=(found-1)->byte;
    return {line,UnitAt(offset)-UnitAt(fLines[line])};
}
Json PositionJSON(TextPosition value) { return {{"line",value.line},{"character",value.character}}; }
TextPosition ReadPosition(const Json& value) {
    if(!value.at("line").is_number_integer() || !value.at("character").is_number_integer()) throw std::runtime_error("Invalid language server position");
    auto line=value.at("line").get<int64_t>(),character=value.at("character").get<int64_t>();
    if(line<0 || character<0 || line>INT32_MAX || character>INT32_MAX) throw std::runtime_error("Invalid language server position");
    return {static_cast<size_t>(line),static_cast<size_t>(character)};
}
std::string FileURI(const std::string& path) {
    static const char hex[]="0123456789ABCDEF";std::string result="file://";
    for(unsigned char c:path) {
        if((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='/' || c=='-' || c=='_' || c=='.' || c=='~') result+=c;
        else { result+='%';result+=hex[c>>4];result+=hex[c&15]; }
    }
    return result;
}
std::vector<DocumentSymbol> ReadSymbols(const Json& result,std::string_view text,const std::string& uri,PositionEncoding encoding) {
    std::vector<DocumentSymbol> symbols;if(!result.is_array()) return symbols;
    PositionMap positions(text,encoding);
    std::function<void(const Json&,int,const std::string&)> append=[&](const Json& rows,int depth,const std::string& container) {
        if(depth>64 || !rows.is_array()) return;
        for(const auto& row:rows) {
            if(symbols.size()>=10000) return;
            try {
                bool flat=row.contains("location");const auto& range=flat?row.at("location").at("range"):row.at("range");
                if(flat && row.at("location").at("uri")!=uri) continue;
                auto selected=flat?range:row.value("selectionRange",range);
                DocumentSymbol symbol{row.at("name").get<std::string>(),row.value("containerName",container),row.value("detail",std::string()),row.value("kind",0),depth,
                    positions.Offset(ReadPosition(range.at("start"))),positions.Offset(ReadPosition(range.at("end"))),
                    positions.Offset(ReadPosition(selected.at("start")))};
                if(symbol.end<symbol.start || symbol.selection<symbol.start || symbol.selection>symbol.end) continue;
                symbols.push_back(symbol);
                if(row.contains("children")) append(row["children"],depth+1,container.empty()?symbol.name:container+" › "+symbol.name);
            } catch(const std::exception&) { /* One malformed symbol must not hide the rest. */ }
        }
    };
    append(result,0,"");return symbols;
}
std::vector<CompletionItem> ReadCompletions(const Json& result) {
    auto rows=result.is_array()?result:result.is_object()?result.value("items",Json::array()):Json::array();
    std::vector<CompletionItem> items;if(!rows.is_array()) return items;
    for(const auto& row:rows) {
        if(items.size()>=10000) break;
        try {
            // We advertise plain text completion; never insert snippet syntax into source.
            if(row.value("insertTextFormat",1)!=1) continue;
            auto label=row.at("label").get<std::string>();if(label.empty()) continue;
            items.push_back({label,row.value("detail",std::string()),row.value("filterText",label),row.value("sortText",label),row});
        } catch(const std::exception&) {}
    }
    std::stable_sort(items.begin(),items.end(),[](const auto& a,const auto& b){return a.sort<b.sort;});return items;
}
std::vector<TextEdit> CompletionEdits(const CompletionItem& item,std::string_view text,size_t wordStart,size_t caret,PositionEncoding encoding) {
    auto value=item.value;std::vector<TextEdit> edits;
    if(value.value("insertTextFormat",1)!=1) throw std::runtime_error("The server returned a snippet despite plain-text completion capabilities");
    if(value.contains("textEdit")) {
        auto edit=value["textEdit"];if(!edit.contains("range")) edit["range"]=edit.at("replace");
        edits.push_back(ReadEdit(edit,text,encoding));
        if(edits[0].start>caret || edits[0].end<caret) throw std::runtime_error("Completion range does not contain the caret");
    } else edits.push_back({wordStart,caret,value.value("insertText",item.label)});
    if(value.contains("additionalTextEdits")) for(const auto& edit:value.at("additionalTextEdits")) edits.push_back(ReadEdit(edit,text,encoding));
    OrderedEdits(edits,text.size());return edits;
}
std::vector<TextEdit> OrderedEdits(std::vector<TextEdit> edits,size_t length) {
    std::stable_sort(edits.begin(),edits.end(),[](const auto& a,const auto& b){return a.start>b.start;});
    for(size_t i=0;i<edits.size();++i) {
        const auto& edit=edits[i];
        if(edit.start>edit.end || edit.end>length || !IsValidUTF8(edit.text) || edit.text.find('\0')!=std::string::npos) throw std::runtime_error("Invalid language server edit");
        if(i && (edit.end>edits[i-1].start || edit.start==edits[i-1].start)) throw std::runtime_error("Overlapping language server edits");
    }
    return edits;
}
size_t MapOffset(size_t offset,const std::vector<TextEdit>& edits) {
    auto ordered=OrderedEdits(edits,SIZE_MAX);int64_t delta=0;
    for(auto it=ordered.rbegin();it!=ordered.rend();++it) {
        if(offset<it->start) break;
        if(offset<=it->end) return static_cast<size_t>(static_cast<int64_t>(it->start)+delta+it->text.size());
        delta+=static_cast<int64_t>(it->text.size())-static_cast<int64_t>(it->end-it->start);
    }
    return static_cast<size_t>(static_cast<int64_t>(offset)+delta);
}
std::string ApplyTextEdits(std::string text,const std::vector<TextEdit>& edits) {
    for(const auto& edit:OrderedEdits(edits,text.size())) text.replace(edit.start,edit.end-edit.start,edit.text);
    return text;
}
const char* SymbolKindName(int kind) {
    static const char* names[]={"Symbol","File","Module","Namespace","Package","Class","Method","Property","Field","Constructor","Enum","Interface","Function","Variable","Constant","String","Number","Boolean","Array","Object","Key","Null","Enum member","Struct","Event","Operator","Type parameter"};
    return kind>=1 && kind<=26?names[kind]:names[0];
}
std::string RpcFramer::Frame(const Json& value) {
    auto body=value.dump();if(body.size()>Limit) throw std::runtime_error("Language server message exceeds 16 MiB");
    return "Content-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
}
std::vector<Json> RpcFramer::Feed(std::string_view bytes) {
    fBuffer.append(bytes);std::vector<Json> messages;
    while(true) {
        if(fHeader) {
            auto end=fBuffer.find("\r\n\r\n");
            if(end==std::string::npos) { if(fBuffer.size()>8192) throw std::runtime_error("Invalid language server header");break; }
            if(end>8192) throw std::runtime_error("Language server header is too long");
            bool found=false;
            for(size_t start=0;start<end;) {
                auto next=fBuffer.find("\r\n",start);auto line=fBuffer.substr(start,next-start);auto colon=line.find(':');
                if(colon==std::string::npos) throw std::runtime_error("Invalid language server header field");
                auto name=line.substr(0,colon);for(auto& c:name) c=std::tolower(static_cast<unsigned char>(c));
                auto value=line.substr(colon+1);while(!value.empty() && value.front()==' ') value.erase(value.begin());
                if(name=="content-length") {
                    if(found || value.empty() || value.find_first_not_of("0123456789")!=std::string::npos || value.size()>9) throw std::runtime_error("Invalid Content-Length");
                    fLength=std::stoul(value);if(fLength==0 || fLength>Limit) throw std::runtime_error("Language server message exceeds 16 MiB");found=true;
                }
                start=next+2;
            }
            if(!found) throw std::runtime_error("Missing Content-Length");
            fBuffer.erase(0,end+4);fHeader=false;
        }
        if(fBuffer.size()<fLength) break;
        auto message=Json::parse(fBuffer.begin(),fBuffer.begin()+fLength);
        if(!message.is_object() || message.value("jsonrpc",std::string())!="2.0") throw std::runtime_error("Invalid JSON-RPC message");
        messages.push_back(std::move(message));fBuffer.erase(0,fLength);fHeader=true;
    }
    return messages;
}
}
