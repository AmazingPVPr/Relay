#include "ChatText.h"
#include "DiscordMarkdown.h"
#include <QRegularExpression>
#include <QUrl>
#include <QDateTime>
#include <QLocale>
#include <QGuiApplication>
#include <QClipboard>
#include <QJsonDocument>
#include <algorithm>
#include <optional>

namespace {
struct MaskedLink { qsizetype end=0;QString label,url,title; };
bool markdownPunctuation(QChar c) {
    const auto n=c.unicode();return (n>=33 && n<=47) || (n>=58 && n<=64) || (n>=91 && n<=96) || (n>=123 && n<=126);
}
QString unescapeDestination(const QString &text) {
    QString result;result.reserve(text.size());
    for(qsizetype i=0;i<text.size();++i){
        if(text[i]=='\\' && i+1<text.size() && markdownPunctuation(text[i+1]))++i;
        result+=text[i];
    }
    return result;
}
std::optional<MaskedLink> maskedLinkAt(const QString &text,qsizetype start) {
    if(text[start]!='[' || DiscordMarkdown::escapedAt(text,start))return {};
    qsizetype at=start+1;int brackets=1;
    const qsizetype labelLimit=std::min(text.size(),start+258);
    for(;at<labelLimit;++at){
        const auto c=text[at];if(c=='\n' || c=='\r')return {};
        if(c=='\\' && at+1<labelLimit && markdownPunctuation(text[at+1])){++at;continue;}
        if(c=='`'){
            const auto end=text.indexOf('`',at+1);
            if(end>at && end<labelLimit){at=end;continue;}
        }
        if(c=='[' && ++brackets>8)return {};
        if(c==']' && --brackets==0)break;
    }
    if(at==start+1 || at>=labelLimit || at+1>=text.size() || text[at+1]!='(')return {};
    MaskedLink link;link.label=text.mid(start+1,at-start-1);at+=2;
    const auto spaces=[&]{while(at<text.size() && (text[at]==' ' || text[at]=='\t'))++at;};
    spaces();const bool angled=at<text.size() && text[at]=='<';if(angled)++at;
    const qsizetype destinationStart=at;int parentheses=0;
    for(;at<text.size() && at-destinationStart<=4096;++at){
        const auto c=text[at];if(c=='\n' || c=='\r')return {};
        if(c=='\\' && at+1<text.size() && markdownPunctuation(text[at+1])){++at;continue;}
        if(angled){if(c=='>')break;if(c=='<')return {};}
        else{
            if(c.isSpace())break;
            if(c=='(' && ++parentheses>16)return {};
            if(c==')'){if(!parentheses)break;--parentheses;}
        }
    }
    if(at==destinationStart || at>=text.size() || at-destinationStart>4096 || parentheses)return {};
    link.url=unescapeDestination(text.mid(destinationStart,at-destinationStart));
    if(angled){if(text[at]!='>')return {};++at;}
    const qsizetype beforeSpaces=at;spaces();
    if(at<text.size() && text[at]!=')'){
        if(at==beforeSpaces)return {};
        const auto delimiter=text[at];if(delimiter!='\"' && delimiter!='\'' && delimiter!='(')return {};
        const auto closing=delimiter=='('?QChar(')'):delimiter;const qsizetype titleStart=++at;
        for(;at<text.size() && at-titleStart<=512;++at){
            const auto c=text[at];if(c=='\n' || c=='\r')return {};
            if(c=='\\' && at+1<text.size() && markdownPunctuation(text[at+1])){++at;continue;}
            if(c==closing)break;
            if(delimiter=='(' && c=='(')return {};
        }
        if(at>=text.size() || at-titleStart>512)return {};
        link.title=unescapeDestination(text.mid(titleStart,at-titleStart));++at;spaces();
    }
    if(at>=text.size() || text[at]!=')')return {};
    link.end=at+1;return link;
}
QString maskedLabel(const QString &label) {
    // Labels may contain ordinary inline formatting and isolated inline code.
    // They never recursively create links or interpret user HTML/mention URLs.
    QString text;QStringList code;qsizetype cursor=0;
    static const QRegularExpression inlineCode("`[^`\\n]*`");
    auto matches=inlineCode.globalMatch(label);
    while(matches.hasNext()){
        const auto match=matches.next();if(DiscordMarkdown::escapedAt(label,match.capturedStart()))continue;
        text+=label.mid(cursor,match.capturedStart()-cursor);
        text+=QChar(0x1f)+QString::number(code.size())+QChar(0x1f);
        code.append("<code style=\"background-color:#252525\">"+match.captured().mid(1,match.capturedLength()-2).toHtmlEscaped()+"</code>");
        cursor=match.capturedEnd();
    }
    text+=label.mid(cursor);return DiscordMarkdown::inlineText(text,code);
}
}

void ChatText::copy(const QString &text) const { if(QGuiApplication::clipboard()) QGuiApplication::clipboard()->setText(text.left(20000)); }

bool ChatText::safeUrl(const QString &value) {
    if (value.size() > 4096 || value.contains(QRegularExpression(QStringLiteral("[\\x00-\\x20\\x7f<>\\\"]")))) return false;
    const QUrl url(value, QUrl::StrictMode);
    return url.isValid() && (url.scheme() == "https" || url.scheme() == "http") && !url.host().isEmpty() && url.userInfo().isEmpty() && url.port() != 0 && !QUrl::fromPercentEncoding(value.toUtf8()).contains(QRegularExpression("[\\x00-\\x1f\\x7f]"));
}
QString ChatText::render(const QString &input, const QVariantMap &metadata) const {
    QString text = input.left(20000);
    const auto mentions=QJsonDocument::fromVariant(QVariantMap{{"users",metadata.value("mentionUsers")},{"roles",metadata.value("mentionRoles")},
        {"underline",metadata.value("underlineLinks",true)},{"contrast",metadata.value("highContrast",false)},
        {"language",metadata.value("language")},{"timeFormat",metadata.value("timeFormat")},
        {"channels",metadata.value("mentionChannels")},{"guild",metadata.value("guildId")}}).toJson(QJsonDocument::Compact);
    const auto localeName=metadata.value("language").toString(); const QLocale locale(localeName=="en_US"||localeName=="en_GB"?localeName:QLocale().name());
    const auto timeFormat=metadata.value("timeFormat").toString();
    const auto timeLabel=[&](const QDateTime &date,bool seconds) {
        if(timeFormat=="12")return locale.toString(date.time(),seconds?"h:mm:ss AP":"h:mm AP");
        if(timeFormat=="24")return locale.toString(date.time(),seconds?"HH:mm:ss":"HH:mm");
        return locale.toString(date.time(),seconds?QLocale::LongFormat:QLocale::ShortFormat);
    };
    const auto key=text+QChar(0)+QString::fromUtf8(mentions)+QChar(0)+locale.name()+QChar(0)+(text.contains("<t:")?QString::number(QDateTime::currentSecsSinceEpoch()/60):QString{});
    if(const auto cached=m_rendered.object(key)) return *cached;
    text.replace(QChar(0x1f),QChar(0xfffd));
    const auto escape = [](const QString &s) { return s.toHtmlEscaped(); };
    const auto link = [&](const QString &url, const QString &label,const QString &title=QString{},bool formatted=false) {
        const auto tooltip=title.isEmpty()?QString{}:QStringLiteral(" title=\"%1\"").arg(escape(title));
        const auto color=metadata.value("highContrast",false).toBool()?QStringLiteral("#9bd7ff"):QStringLiteral("#83bcf1");
        const auto decoration=metadata.value("underlineLinks",true).toBool()?QStringLiteral("underline"):QStringLiteral("none");
        return QStringLiteral("<a href=\"%1\"%2 style=\"color:%3;text-decoration:%4\">%5</a>").arg(escape(url),tooltip,color,decoration,formatted?maskedLabel(label):escape(label));
    };
    QHash<QString, QString> users, roles;
    for (const auto &v : metadata.value("mentionUsers").toList()) {
        const auto m = v.toMap(); users.insert(m.value("id").toString(), m.value("name").toString().left(128));
    }
    for (const auto &v : metadata.value("mentionRoles").toList()) {
        const auto m = v.toMap(); roles.insert(m.value("id").toString(), m.value("name").toString().left(128));
    }
    QHash<QString,QVariantMap> channels;
    for(const auto &value:metadata.value("mentionChannels").toList()){
        const auto channel=value.toMap();channels.insert(channel.value("id").toString(),channel);
    }
    // Protect validated links, mentions and code before applying Discord formatting.
    static const QRegularExpression token(QStringLiteral(R"re(```[\s\S]*?```|`[^`\n]*`|\[|https?://[^\s<>]+|</[A-Za-z0-9_ -]{1,100}:[0-9]{1,20}>|<t:[0-9]{1,12}(?::[tTdDfFR])?>|<a?:[A-Za-z0-9_]{1,32}:[0-9]{1,20}>|<#[0-9]{1,20}>|<id:(?:customize|browse)>|<@!?[0-9]{1,20}>|<@&[0-9]{1,20}>|@everyone\b|@here\b)re"));
    static const QRegularExpression userToken(QStringLiteral("^<@!?([0-9]{1,20})>$"));
    static const QRegularExpression roleToken(QStringLiteral("^<@&([0-9]{1,20})>$"));
    static const QRegularExpression timestampToken(QStringLiteral("^<t:([0-9]{1,12})(?::([tTdDfFR]))?>$"));
    static const QRegularExpression emojiToken(QStringLiteral("^<a?:([A-Za-z0-9_]{1,32}):([0-9]{1,20})>$"));
    static const QRegularExpression commandToken(QStringLiteral("^</([A-Za-z0-9_ -]{1,100}):[0-9]{1,20}>$"));
    static const QRegularExpression channelToken(QStringLiteral("^<#([0-9]{1,20})>$"));
    static const QRegularExpression guildToken(QStringLiteral("^<id:(customize|browse)>$"));
    static const QRegularExpression guildIdPattern(QStringLiteral("^[0-9]{1,20}$"));
    QString result; QStringList tokens; qsizetype cursor = 0;
    qsizetype search=0;
    while(search<text.size()) {
        const auto match=token.match(text,search);if(!match.hasMatch())break;
        if((match.captured().startsWith("<#") || match.captured().startsWith("<id:")) && DiscordMarkdown::escapedAt(text,match.capturedStart())){search=match.capturedEnd();continue;}
        const auto masked=match.captured()=="["?maskedLinkAt(text,match.capturedStart()):std::optional<MaskedLink>{};
        if(match.captured()=="[" && !masked){search=match.capturedEnd();continue;}
        result += text.mid(cursor, match.capturedStart() - cursor);
        const auto htmlStart=result.size();
        cursor=masked?masked->end:match.capturedEnd();search=cursor;
        QString value = text.mid(match.capturedStart(),cursor-match.capturedStart());
        QString formattingSuffix;
        if(value.startsWith("http"))for(const auto &delimiter:QStringList{"**","__","~~","*","_"}){
            if(value.endsWith(delimiter) && text.left(match.capturedStart()).count(delimiter)%2){value.chop(delimiter.size());formattingSuffix.prepend(delimiter);}
        }
        const auto u = userToken.match(value), r = roleToken.match(value), stamp = timestampToken.match(value), emoji = emojiToken.match(value), command = commandToken.match(value);
        if(value.startsWith("```")){
            auto code=value.mid(3,value.size()-6);const auto newline=code.indexOf('\n');
            if(newline>=0 && QRegularExpression("^[A-Za-z0-9_+\\-]{0,32}$").match(code.left(newline)).hasMatch())code.remove(0,newline+1);
            result+="<pre style=\"background-color:#252525\">"+escape(code)+"</pre>";
        }
        else if (value.startsWith('`')) result += "<code style=\"background-color:#252525\">" + escape(value.mid(1,value.size()-2)) + "</code>";
        else if(const auto channel=channelToken.match(value);channel.hasMatch()){
            const auto id=channel.captured(1);const auto info=channels.value(id);
            const auto name=info.value("name").toString().left(100);
            const bool visible=info.value("accessible",!name.isEmpty()).toBool() && !name.isEmpty();
            const auto label=visible?"#"+name:QStringLiteral("#Unknown channel");
            if(visible || !info.value("known",false).toBool())
                result+="<a href=\"relay-channel:"+id+"\" style=\"color:#c9c4ff;background-color:#353047;text-decoration:none\">"+escape(label)+"</a>";
            else result+="<span style=\"color:#a7a7af;background-color:#303030\">"+escape(label)+"</span>";
        }
        else if(const auto navigation=guildToken.match(value);navigation.hasMatch()){
            const auto kind=navigation.captured(1),guild=metadata.value("guildId").toString();
            const auto label=kind=="customize"?QStringLiteral("Channels & Roles"):QStringLiteral("Browse Channels");
            if(guildIdPattern.match(guild).hasMatch())
                result+="<a href=\"relay-guild:"+guild+":"+kind+"\" style=\"color:#c9c4ff;background-color:#353047;text-decoration:none\">"+escape(label)+"</a>";
            else result+="<span style=\"color:#c9c4ff;background-color:#353047\">"+escape(label)+"</span>";
        }
        else if (command.hasMatch()) result += "<span style=\"color:#c9c4ff;background-color:#353047\">/"+escape(command.captured(1))+"</span>";
        else if (stamp.hasMatch()) {
            const auto date = QDateTime::fromSecsSinceEpoch(stamp.captured(1).toLongLong()).toLocalTime();
            const auto format = stamp.captured(2); QString label;
            if(format == "t") label=timeLabel(date,false);
            else if(format == "T") label=timeLabel(date,true);
            else if(format == "d") label=locale.toString(date.date(),QLocale::ShortFormat);
            else if(format == "D") label=locale.toString(date.date(),QLocale::LongFormat);
            else if(format == "R") {
                const auto delta=QDateTime::currentSecsSinceEpoch()-date.toSecsSinceEpoch(); const auto seconds=qAbs(delta);
                const auto count=seconds<60?seconds:seconds<3600?seconds/60:seconds<86400?seconds/3600:seconds/86400;
                const auto unit=seconds<60?"second":seconds<3600?"minute":seconds<86400?"hour":"day";
                label=QString("%1 %2%3").arg(count).arg(unit).arg(count==1?"":"s");
                label=seconds<5?"just now":delta>=0?label+" ago":"in "+label;
            } else label=locale.toString(date.date(),format=="F"?QLocale::LongFormat:QLocale::ShortFormat)+" "+timeLabel(date,false);
            result += "<span style=\"background-color:#303030\">"+escape(label)+"</span>";
        } else if (emoji.hasMatch()) {
            result += QStringLiteral("<img src=\"https://cdn.discordapp.com/emojis/%1.png?size=128\" width=\"22\" height=\"22\" alt=\":%2:\"/>").arg(emoji.captured(2),escape(emoji.captured(1)));
        } else if (u.hasMatch()) {
            const auto id = u.captured(1); const auto name = users.value(id, "Unknown user");
            result += QStringLiteral("<a href=\"relay-user:%1\" style=\"color:#c9c4ff;background-color:#353047;text-decoration:none\">@%2</a>").arg(id, escape(name));
        } else if (r.hasMatch() || value == "@everyone" || value == "@here") {
            const auto label = r.hasMatch() ? "@" + roles.value(r.captured(1), "Unknown role") : value;
            result += "<span style=\"color:#c9c4ff;background-color:#353047\">" + escape(label) + "</span>";
        } else if (masked) result += safeUrl(masked->url)?link(masked->url,masked->label,masked->title,true):escape(value);
        else if (value.startsWith("http")) {
            QString suffix;
            while (!value.isEmpty()) {
                const QChar end = value.back();
                const bool excessClose = (end == ')' && value.count(')') > value.count('(')) || (end == ']' && value.count(']') > value.count('['));
                if (!QStringLiteral(".,!?;:").contains(end) && !excessClose) break;
                suffix.prepend(end); value.chop(1);
            }
            result += (safeUrl(value) ? link(value, value) : escape(value)) + escape(suffix);
        } else result += escape(value);
        const auto html=result.mid(htmlStart);result.truncate(htmlStart);
        result+=QChar(0x1f)+QString::number(tokens.size())+QChar(0x1f);tokens.append(html);
        result+=formattingSuffix;
    }
    result += text.mid(cursor);
    auto html=DiscordMarkdown::render(result,tokens);
    m_rendered.insert(key,new QString(html),qMax(1,int((key.size()+html.size())*sizeof(QChar)/1024)));
    return html;
}
