#include "WindowsGlobalShortcuts.h"
#include <QCoreApplication>
#include <QKeySequence>
#include <QSet>
#include <windows.h>
namespace {
UINT virtualKey(Qt::Key key) {
    if(key>=Qt::Key_A && key<=Qt::Key_Z)return 'A'+key-Qt::Key_A;
    if(key>=Qt::Key_0 && key<=Qt::Key_9)return '0'+key-Qt::Key_0;
    if(key>=Qt::Key_F1 && key<=Qt::Key_F24)return VK_F1+key-Qt::Key_F1;
    switch(key){
    case Qt::Key_Space:return VK_SPACE;case Qt::Key_Return:return VK_RETURN;
    case Qt::Key_Tab:return VK_TAB;case Qt::Key_Escape:return VK_ESCAPE;
    case Qt::Key_Backspace:return VK_BACK;case Qt::Key_Delete:return VK_DELETE;
    case Qt::Key_Insert:return VK_INSERT;case Qt::Key_Home:return VK_HOME;
    case Qt::Key_End:return VK_END;case Qt::Key_Left:return VK_LEFT;
    case Qt::Key_Right:return VK_RIGHT;case Qt::Key_Up:return VK_UP;
    case Qt::Key_Down:return VK_DOWN;case Qt::Key_PageUp:return VK_PRIOR;
    case Qt::Key_PageDown:return VK_NEXT;default:return 0;
    }
}
}
GlobalShortcuts::GlobalShortcuts(QObject *parent):QObject(parent){QCoreApplication::instance()->installNativeEventFilter(this);}
GlobalShortcuts::~GlobalShortcuts(){release();QCoreApplication::instance()->removeNativeEventFilter(this);}
QString GlobalShortcuts::preferredTrigger(const QString &sequence){return QKeySequence::fromString(sequence,QKeySequence::PortableText).toString(QKeySequence::PortableText);}
void GlobalShortcuts::release(){
    for(auto it=m_actions.cbegin();it!=m_actions.cend();++it){UnregisterHotKey(nullptr,it.key());GlobalDeleteAtom(ATOM(it.key()));}
    m_actions.clear();m_bindings.clear();
}
void GlobalShortcuts::disable(){release();m_error.clear();emit changed();}
void GlobalShortcuts::enable(const QVariantList &bindings){
    release();m_error.clear();QSet<QString> seen;
    for(const auto &value:bindings){
        const auto row=value.toMap();const auto action=row.value("action").toString(),raw=row.value("sequence").toString();
        if(!QStringList{"toggle-mute","toggle-deafen","disconnect-voice"}.contains(action) || seen.contains(action) || raw.trimmed().isEmpty())continue;
        seen.insert(action);
        const auto sequence=QKeySequence::fromString(raw,QKeySequence::PortableText);
        const auto key=sequence.count()==1?virtualKey(sequence[0].key()):0;
        const auto modifiers=sequence.count()==1?sequence[0].keyboardModifiers():Qt::KeyboardModifiers{};
        UINT flags=MOD_NOREPEAT;
        if(modifiers&Qt::ControlModifier)flags|=MOD_CONTROL;
        if(modifiers&Qt::AltModifier)flags|=MOD_ALT;
        if(modifiers&Qt::ShiftModifier)flags|=MOD_SHIFT;
        if(modifiers&Qt::MetaModifier)flags|=MOD_WIN;
        const auto atom=key?GlobalAddAtomW(reinterpret_cast<LPCWSTR>((QStringLiteral("Relay.")+QString::number(QCoreApplication::applicationPid())+'.'+action).utf16())):0;
        if(!atom || !RegisterHotKey(nullptr,atom,flags,key)){
            if(atom)GlobalDeleteAtom(atom);
            release();m_error="Windows could not assign a shortcut. It may already be in use, reserved, or unsupported. Choose another key.";emit changed();return;
        }
        m_actions.insert(atom,action);
        m_bindings.append(QVariantMap{{"action",action},{"label",row.value("label",action)},{"trigger",sequence.toString(QKeySequence::NativeText)}});
    }
    if(m_actions.isEmpty())m_error="Choose at least one voice shortcut before enabling system shortcuts.";
    emit changed();
}
bool GlobalShortcuts::nativeEventFilter(const QByteArray &,void *message,qintptr *result){
    const auto *event=static_cast<MSG*>(message);
    if(event->message!=WM_HOTKEY || !m_actions.contains(int(event->wParam)))return false;
    const auto action=m_actions.value(int(event->wParam));*result=0;emit actionRequested(action);return true;
}
