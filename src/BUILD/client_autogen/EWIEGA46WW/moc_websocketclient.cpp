/****************************************************************************
** Meta object code from reading C++ file 'websocketclient.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../websocketclient.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#include <QtCore/QList>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'websocketclient.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_WebSocketClient_t {
    QByteArrayData data[20];
    char stringdata0[280];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_WebSocketClient_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_WebSocketClient_t qt_meta_stringdata_WebSocketClient = {
    {
QT_MOC_LITERAL(0, 0, 15), // "WebSocketClient"
QT_MOC_LITERAL(1, 16, 6), // "closed"
QT_MOC_LITERAL(2, 23, 0), // ""
QT_MOC_LITERAL(3, 24, 15), // "messageReceived"
QT_MOC_LITERAL(4, 40, 7), // "message"
QT_MOC_LITERAL(5, 48, 15), // "connectionError"
QT_MOC_LITERAL(6, 64, 5), // "error"
QT_MOC_LITERAL(7, 70, 23), // "connectionStatusChanged"
QT_MOC_LITERAL(8, 94, 11), // "isConnected"
QT_MOC_LITERAL(9, 106, 6), // "status"
QT_MOC_LITERAL(10, 113, 16), // "onHttpsConnected"
QT_MOC_LITERAL(11, 130, 19), // "onHttpsDisconnected"
QT_MOC_LITERAL(12, 150, 15), // "onHttpConnected"
QT_MOC_LITERAL(13, 166, 18), // "onHttpDisconnected"
QT_MOC_LITERAL(14, 185, 21), // "onTextMessageReceived"
QT_MOC_LITERAL(15, 207, 7), // "onError"
QT_MOC_LITERAL(16, 215, 28), // "QAbstractSocket::SocketError"
QT_MOC_LITERAL(17, 244, 11), // "onSslErrors"
QT_MOC_LITERAL(18, 256, 16), // "QList<QSslError>"
QT_MOC_LITERAL(19, 273, 6) // "errors"

    },
    "WebSocketClient\0closed\0\0messageReceived\0"
    "message\0connectionError\0error\0"
    "connectionStatusChanged\0isConnected\0"
    "status\0onHttpsConnected\0onHttpsDisconnected\0"
    "onHttpConnected\0onHttpDisconnected\0"
    "onTextMessageReceived\0onError\0"
    "QAbstractSocket::SocketError\0onSslErrors\0"
    "QList<QSslError>\0errors"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_WebSocketClient[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      11,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       4,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,   69,    2, 0x06 /* Public */,
       3,    1,   70,    2, 0x06 /* Public */,
       5,    1,   73,    2, 0x06 /* Public */,
       7,    2,   76,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      10,    0,   81,    2, 0x08 /* Private */,
      11,    0,   82,    2, 0x08 /* Private */,
      12,    0,   83,    2, 0x08 /* Private */,
      13,    0,   84,    2, 0x08 /* Private */,
      14,    1,   85,    2, 0x08 /* Private */,
      15,    1,   88,    2, 0x08 /* Private */,
      17,    1,   91,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,    4,
    QMetaType::Void, QMetaType::QString,    6,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,    8,    9,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,    4,
    QMetaType::Void, 0x80000000 | 16,    6,
    QMetaType::Void, 0x80000000 | 18,   19,

       0        // eod
};

void WebSocketClient::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<WebSocketClient *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->closed(); break;
        case 1: _t->messageReceived((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 2: _t->connectionError((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 3: _t->connectionStatusChanged((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 4: _t->onHttpsConnected(); break;
        case 5: _t->onHttpsDisconnected(); break;
        case 6: _t->onHttpConnected(); break;
        case 7: _t->onHttpDisconnected(); break;
        case 8: _t->onTextMessageReceived((*reinterpret_cast< QString(*)>(_a[1]))); break;
        case 9: _t->onError((*reinterpret_cast< QAbstractSocket::SocketError(*)>(_a[1]))); break;
        case 10: _t->onSslErrors((*reinterpret_cast< const QList<QSslError>(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        switch (_id) {
        default: *reinterpret_cast<int*>(_a[0]) = -1; break;
        case 9:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< QAbstractSocket::SocketError >(); break;
            }
            break;
        case 10:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< QList<QSslError> >(); break;
            }
            break;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (WebSocketClient::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&WebSocketClient::closed)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (WebSocketClient::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&WebSocketClient::messageReceived)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (WebSocketClient::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&WebSocketClient::connectionError)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (WebSocketClient::*)(bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&WebSocketClient::connectionStatusChanged)) {
                *result = 3;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject WebSocketClient::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_WebSocketClient.data,
    qt_meta_data_WebSocketClient,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *WebSocketClient::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *WebSocketClient::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_WebSocketClient.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int WebSocketClient::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 11)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 11;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 11)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 11;
    }
    return _id;
}

// SIGNAL 0
void WebSocketClient::closed()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void WebSocketClient::messageReceived(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void WebSocketClient::connectionError(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void WebSocketClient::connectionStatusChanged(bool _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
