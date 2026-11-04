#pragma once
#include <QString>
#include <QStringList>
#include <QRegularExpression>
#include <array>

namespace DiscordMarkdown {
inline bool escapedAt(const QString &text,qsizetype position) {
    int slashes=0;while(position>0 && text[--position]=='\\')++slashes;return slashes%2;
}
inline QString inlineText(const QString &text,const QStringList &tokens,int depth=0) {
    QString out;out.reserve(text.size()*2);
    static const std::array<QString,6> delimiters{"***","**","__","~~","*","_"};
    const QStringView view(text);
    for(qsizetype i=0;i<text.size();) {
        if(text[i]==QChar(0x1f)) {
            const auto end=text.indexOf(QChar(0x1f),i+1);bool valid=false;
            const int id=end>i?text.mid(i+1,end-i-1).toInt(&valid):-1;
            if(valid && id>=0 && id<tokens.size()){out+=tokens[id];i=end+1;continue;}
        }
        if(text[i]=='\\' && i+1<text.size() && QStringLiteral("\\`*_~|>[]()#-").contains(text[i+1])){out+=QString(text[i+1]).toHtmlEscaped();i+=2;continue;}
        bool formatted=false;
        if(depth<8 && (text[i]=='*' || text[i]=='_' || text[i]=='~'))for(const auto &delimiter:delimiters) {
            if(i+delimiter.size()>=text.size() || view.sliced(i,delimiter.size())!=delimiter || text[i+delimiter.size()].isSpace())continue;
            if(delimiter.contains('_') && i>0 && text[i-1].isLetterOrNumber())continue;
            auto end=text.indexOf(delimiter,i+delimiter.size());
            while(end>=0 && (escapedAt(text,end) || text[end-1].isSpace() || (delimiter.contains('_') && end+delimiter.size()<text.size() && text[end+delimiter.size()].isLetterOrNumber())))end=text.indexOf(delimiter,end+1);
            if(end<0)continue;
            const QString tag=delimiter=="***"?"b><i":delimiter=="**"?"b":delimiter=="__"?"u":delimiter=="~~"?"s":"i";
            const QString close=delimiter=="***"?"i></b":tag;
            out+='<'+tag+'>'+inlineText(text.mid(i+delimiter.size(),end-i-delimiter.size()),tokens,depth+1)+"</"+close+'>';
            i=end+delimiter.size();formatted=true;break;
        }
        if(formatted)continue;
        const auto c=text[i++];
        if(c=='&')out+="&amp;";
        else if(c=='<')out+="&lt;";
        else if(c=='>')out+="&gt;";
        else if(c=='\"')out+="&quot;";
        else out+=c;
    }
    return out;
}
inline QString render(const QString &text,const QStringList &tokens) {
    QStringList lines;
    static const QRegularExpression headingPattern("^(#{1,3}) (.+)$");
    for(const auto &line:text.split('\n')) {
        const auto heading=headingPattern.match(line);
        if(heading.hasMatch())lines+="<b><span style=\"font-size:large\">"+inlineText(heading.captured(2),tokens)+"</span></b>";
        else if(line.startsWith("> "))lines+="<span style=\"color:#b0b0b0\">│ "+inlineText(line.mid(2),tokens)+"</span>";
        else if(line.startsWith("- ") || line.startsWith("* "))lines+="• "+inlineText(line.mid(2),tokens);
        else lines+=inlineText(line,tokens);
    }
    return "<span style=\"white-space:pre-wrap\">"+lines.join("<br/>")+"</span>";
}
}
