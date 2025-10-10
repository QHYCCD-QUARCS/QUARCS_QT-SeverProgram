/****************************************************************************
** Meta object code from reading C++ file 'autofocus.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../autofocus.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'autofocus.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_AutoFocus_t {
    QByteArrayData data[37];
    char stringdata0[430];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_AutoFocus_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_AutoFocus_t qt_meta_stringdata_AutoFocus = {
    {
QT_MOC_LITERAL(0, 0, 9), // "AutoFocus"
QT_MOC_LITERAL(1, 10, 12), // "stateChanged"
QT_MOC_LITERAL(2, 23, 0), // ""
QT_MOC_LITERAL(3, 24, 14), // "AutoFocusState"
QT_MOC_LITERAL(4, 39, 8), // "newState"
QT_MOC_LITERAL(5, 48, 15), // "progressUpdated"
QT_MOC_LITERAL(6, 64, 8), // "progress"
QT_MOC_LITERAL(7, 73, 10), // "logMessage"
QT_MOC_LITERAL(8, 84, 7), // "message"
QT_MOC_LITERAL(9, 92, 18), // "autoFocusCompleted"
QT_MOC_LITERAL(10, 111, 7), // "success"
QT_MOC_LITERAL(11, 119, 12), // "bestPosition"
QT_MOC_LITERAL(12, 132, 6), // "minHFR"
QT_MOC_LITERAL(13, 139, 13), // "errorOccurred"
QT_MOC_LITERAL(14, 153, 5), // "error"
QT_MOC_LITERAL(15, 159, 20), // "captureStatusChanged"
QT_MOC_LITERAL(16, 180, 10), // "isComplete"
QT_MOC_LITERAL(17, 191, 9), // "imagePath"
QT_MOC_LITERAL(18, 201, 14), // "roiInfoChanged"
QT_MOC_LITERAL(19, 216, 3), // "roi"
QT_MOC_LITERAL(20, 220, 16), // "focusSeriesReset"
QT_MOC_LITERAL(21, 237, 5), // "stage"
QT_MOC_LITERAL(22, 243, 19), // "focusDataPointReady"
QT_MOC_LITERAL(23, 263, 8), // "position"
QT_MOC_LITERAL(24, 272, 3), // "hfr"
QT_MOC_LITERAL(25, 276, 15), // "focusFitUpdated"
QT_MOC_LITERAL(26, 292, 1), // "a"
QT_MOC_LITERAL(27, 294, 1), // "b"
QT_MOC_LITERAL(28, 296, 1), // "c"
QT_MOC_LITERAL(29, 298, 24), // "startPositionUpdateTimer"
QT_MOC_LITERAL(30, 323, 20), // "focusBestMovePlanned"
QT_MOC_LITERAL(31, 344, 15), // "autofocusFailed"
QT_MOC_LITERAL(32, 360, 19), // "starDetectionResult"
QT_MOC_LITERAL(33, 380, 8), // "detected"
QT_MOC_LITERAL(34, 389, 20), // "autoFocusModeChanged"
QT_MOC_LITERAL(35, 410, 4), // "mode"
QT_MOC_LITERAL(36, 415, 14) // "onTimerTimeout"

    },
    "AutoFocus\0stateChanged\0\0AutoFocusState\0"
    "newState\0progressUpdated\0progress\0"
    "logMessage\0message\0autoFocusCompleted\0"
    "success\0bestPosition\0minHFR\0errorOccurred\0"
    "error\0captureStatusChanged\0isComplete\0"
    "imagePath\0roiInfoChanged\0roi\0"
    "focusSeriesReset\0stage\0focusDataPointReady\0"
    "position\0hfr\0focusFitUpdated\0a\0b\0c\0"
    "startPositionUpdateTimer\0focusBestMovePlanned\0"
    "autofocusFailed\0starDetectionResult\0"
    "detected\0autoFocusModeChanged\0mode\0"
    "onTimerTimeout"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_AutoFocus[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      16,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
      15,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   94,    2, 0x06 /* Public */,
       5,    1,   97,    2, 0x06 /* Public */,
       7,    1,  100,    2, 0x06 /* Public */,
       9,    3,  103,    2, 0x06 /* Public */,
      13,    1,  110,    2, 0x06 /* Public */,
      15,    2,  113,    2, 0x06 /* Public */,
      18,    1,  118,    2, 0x06 /* Public */,
      20,    1,  121,    2, 0x06 /* Public */,
      22,    3,  124,    2, 0x06 /* Public */,
      25,    5,  131,    2, 0x06 /* Public */,
      29,    0,  142,    2, 0x06 /* Public */,
      30,    2,  143,    2, 0x06 /* Public */,
      31,    0,  148,    2, 0x06 /* Public */,
      32,    2,  149,    2, 0x06 /* Public */,
      34,    2,  154,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      36,    0,  159,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, 0x80000000 | 3,    4,
    QMetaType::Void, QMetaType::Int,    6,
    QMetaType::Void, QMetaType::QString,    8,
    QMetaType::Void, QMetaType::Bool, QMetaType::Double, QMetaType::Double,   10,   11,   12,
    QMetaType::Void, QMetaType::QString,   14,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,   16,   17,
    QMetaType::Void, QMetaType::QRect,   19,
    QMetaType::Void, QMetaType::QString,   21,
    QMetaType::Void, QMetaType::Int, QMetaType::Double, QMetaType::QString,   23,   24,   21,
    QMetaType::Void, QMetaType::Double, QMetaType::Double, QMetaType::Double, QMetaType::Double, QMetaType::Double,   26,   27,   28,   11,   12,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int, QMetaType::QString,   11,   21,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool, QMetaType::Double,   33,   24,
    QMetaType::Void, QMetaType::QString, QMetaType::Double,   35,   24,

 // slots: parameters
    QMetaType::Void,

       0        // eod
};

void AutoFocus::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<AutoFocus *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->stateChanged((*reinterpret_cast< AutoFocusState(*)>(_a[1]))); break;
        case 1: _t->progressUpdated((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 2: _t->logMessage((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 3: _t->autoFocusCompleted((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< double(*)>(_a[2])),(*reinterpret_cast< double(*)>(_a[3]))); break;
        case 4: _t->errorOccurred((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 5: _t->captureStatusChanged((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 6: _t->roiInfoChanged((*reinterpret_cast< const QRect(*)>(_a[1]))); break;
        case 7: _t->focusSeriesReset((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 8: _t->focusDataPointReady((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< double(*)>(_a[2])),(*reinterpret_cast< const QString(*)>(_a[3]))); break;
        case 9: _t->focusFitUpdated((*reinterpret_cast< double(*)>(_a[1])),(*reinterpret_cast< double(*)>(_a[2])),(*reinterpret_cast< double(*)>(_a[3])),(*reinterpret_cast< double(*)>(_a[4])),(*reinterpret_cast< double(*)>(_a[5]))); break;
        case 10: _t->startPositionUpdateTimer(); break;
        case 11: _t->focusBestMovePlanned((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 12: _t->autofocusFailed(); break;
        case 13: _t->starDetectionResult((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< double(*)>(_a[2]))); break;
        case 14: _t->autoFocusModeChanged((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< double(*)>(_a[2]))); break;
        case 15: _t->onTimerTimeout(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (AutoFocus::*)(AutoFocusState );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AutoFocus::stateChanged)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (AutoFocus::*)(int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AutoFocus::progressUpdated)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (AutoFocus::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AutoFocus::logMessage)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (AutoFocus::*)(bool , double , double );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AutoFocus::autoFocusCompleted)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (AutoFocus::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AutoFocus::errorOccurred)) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (AutoFocus::*)(bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AutoFocus::captureStatusChanged)) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (AutoFocus::*)(const QRect & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AutoFocus::roiInfoChanged)) {
                *result = 6;
                return;
            }
        }
        {
            using _t = void (AutoFocus::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AutoFocus::focusSeriesReset)) {
                *result = 7;
                return;
            }
        }
        {
            using _t = void (AutoFocus::*)(int , double , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AutoFocus::focusDataPointReady)) {
                *result = 8;
                return;
            }
        }
        {
            using _t = void (AutoFocus::*)(double , double , double , double , double );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AutoFocus::focusFitUpdated)) {
                *result = 9;
                return;
            }
        }
        {
            using _t = void (AutoFocus::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AutoFocus::startPositionUpdateTimer)) {
                *result = 10;
                return;
            }
        }
        {
            using _t = void (AutoFocus::*)(int , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AutoFocus::focusBestMovePlanned)) {
                *result = 11;
                return;
            }
        }
        {
            using _t = void (AutoFocus::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AutoFocus::autofocusFailed)) {
                *result = 12;
                return;
            }
        }
        {
            using _t = void (AutoFocus::*)(bool , double );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AutoFocus::starDetectionResult)) {
                *result = 13;
                return;
            }
        }
        {
            using _t = void (AutoFocus::*)(const QString & , double );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AutoFocus::autoFocusModeChanged)) {
                *result = 14;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject AutoFocus::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_AutoFocus.data,
    qt_meta_data_AutoFocus,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *AutoFocus::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *AutoFocus::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_AutoFocus.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int AutoFocus::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 16)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 16;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 16)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 16;
    }
    return _id;
}

// SIGNAL 0
void AutoFocus::stateChanged(AutoFocusState _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void AutoFocus::progressUpdated(int _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void AutoFocus::logMessage(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void AutoFocus::autoFocusCompleted(bool _t1, double _t2, double _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void AutoFocus::errorOccurred(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}

// SIGNAL 5
void AutoFocus::captureStatusChanged(bool _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 5, _a);
}

// SIGNAL 6
void AutoFocus::roiInfoChanged(const QRect & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 6, _a);
}

// SIGNAL 7
void AutoFocus::focusSeriesReset(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 7, _a);
}

// SIGNAL 8
void AutoFocus::focusDataPointReady(int _t1, double _t2, const QString & _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))) };
    QMetaObject::activate(this, &staticMetaObject, 8, _a);
}

// SIGNAL 9
void AutoFocus::focusFitUpdated(double _t1, double _t2, double _t3, double _t4, double _t5)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t4))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t5))) };
    QMetaObject::activate(this, &staticMetaObject, 9, _a);
}

// SIGNAL 10
void AutoFocus::startPositionUpdateTimer()
{
    QMetaObject::activate(this, &staticMetaObject, 10, nullptr);
}

// SIGNAL 11
void AutoFocus::focusBestMovePlanned(int _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 11, _a);
}

// SIGNAL 12
void AutoFocus::autofocusFailed()
{
    QMetaObject::activate(this, &staticMetaObject, 12, nullptr);
}

// SIGNAL 13
void AutoFocus::starDetectionResult(bool _t1, double _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 13, _a);
}

// SIGNAL 14
void AutoFocus::autoFocusModeChanged(const QString & _t1, double _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 14, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
