/****************************************************************************
** Meta object code from reading C++ file 'autopolaralignment.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../autopolaralignment.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'autopolaralignment.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_PolarAlignment_t {
    QByteArrayData data[28];
    char stringdata0[331];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_PolarAlignment_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_PolarAlignment_t qt_meta_stringdata_PolarAlignment = {
    {
QT_MOC_LITERAL(0, 0, 14), // "PolarAlignment"
QT_MOC_LITERAL(1, 15, 12), // "stateChanged"
QT_MOC_LITERAL(2, 28, 0), // ""
QT_MOC_LITERAL(3, 29, 19), // "PolarAlignmentState"
QT_MOC_LITERAL(4, 49, 8), // "newState"
QT_MOC_LITERAL(5, 58, 7), // "message"
QT_MOC_LITERAL(6, 66, 10), // "percentage"
QT_MOC_LITERAL(7, 77, 19), // "adjustmentGuideData"
QT_MOC_LITERAL(8, 97, 2), // "ra"
QT_MOC_LITERAL(9, 100, 3), // "dec"
QT_MOC_LITERAL(10, 104, 5), // "maxRa"
QT_MOC_LITERAL(11, 110, 5), // "minRa"
QT_MOC_LITERAL(12, 116, 6), // "maxDec"
QT_MOC_LITERAL(13, 123, 6), // "minDec"
QT_MOC_LITERAL(14, 130, 8), // "targetRa"
QT_MOC_LITERAL(15, 139, 9), // "targetDec"
QT_MOC_LITERAL(16, 149, 8), // "offsetRa"
QT_MOC_LITERAL(17, 158, 9), // "offsetDec"
QT_MOC_LITERAL(18, 168, 12), // "adjustmentRa"
QT_MOC_LITERAL(19, 181, 13), // "adjustmentDec"
QT_MOC_LITERAL(20, 195, 11), // "resultReady"
QT_MOC_LITERAL(21, 207, 20), // "PolarAlignmentResult"
QT_MOC_LITERAL(22, 228, 6), // "result"
QT_MOC_LITERAL(23, 235, 13), // "errorOccurred"
QT_MOC_LITERAL(24, 249, 5), // "error"
QT_MOC_LITERAL(25, 255, 19), // "onStateTimerTimeout"
QT_MOC_LITERAL(26, 275, 32), // "onCaptureAndAnalysisTimerTimeout"
QT_MOC_LITERAL(27, 308, 22) // "onMovementTimerTimeout"

    },
    "PolarAlignment\0stateChanged\0\0"
    "PolarAlignmentState\0newState\0message\0"
    "percentage\0adjustmentGuideData\0ra\0dec\0"
    "maxRa\0minRa\0maxDec\0minDec\0targetRa\0"
    "targetDec\0offsetRa\0offsetDec\0adjustmentRa\0"
    "adjustmentDec\0resultReady\0"
    "PolarAlignmentResult\0result\0errorOccurred\0"
    "error\0onStateTimerTimeout\0"
    "onCaptureAndAnalysisTimerTimeout\0"
    "onMovementTimerTimeout"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_PolarAlignment[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       7,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       4,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    3,   49,    2, 0x06 /* Public */,
       7,   12,   56,    2, 0x06 /* Public */,
      20,    1,   81,    2, 0x06 /* Public */,
      23,    1,   84,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      25,    0,   87,    2, 0x08 /* Private */,
      26,    0,   88,    2, 0x08 /* Private */,
      27,    0,   89,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, 0x80000000 | 3, QMetaType::QString, QMetaType::Int,    4,    5,    6,
    QMetaType::Void, QMetaType::Double, QMetaType::Double, QMetaType::Double, QMetaType::Double, QMetaType::Double, QMetaType::Double, QMetaType::Double, QMetaType::Double, QMetaType::Double, QMetaType::Double, QMetaType::QString, QMetaType::QString,    8,    9,   10,   11,   12,   13,   14,   15,   16,   17,   18,   19,
    QMetaType::Void, 0x80000000 | 21,   22,
    QMetaType::Void, QMetaType::QString,   24,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

void PolarAlignment::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<PolarAlignment *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->stateChanged((*reinterpret_cast< PolarAlignmentState(*)>(_a[1])),(*reinterpret_cast< QString(*)>(_a[2])),(*reinterpret_cast< int(*)>(_a[3]))); break;
        case 1: _t->adjustmentGuideData((*reinterpret_cast< double(*)>(_a[1])),(*reinterpret_cast< double(*)>(_a[2])),(*reinterpret_cast< double(*)>(_a[3])),(*reinterpret_cast< double(*)>(_a[4])),(*reinterpret_cast< double(*)>(_a[5])),(*reinterpret_cast< double(*)>(_a[6])),(*reinterpret_cast< double(*)>(_a[7])),(*reinterpret_cast< double(*)>(_a[8])),(*reinterpret_cast< double(*)>(_a[9])),(*reinterpret_cast< double(*)>(_a[10])),(*reinterpret_cast< QString(*)>(_a[11])),(*reinterpret_cast< QString(*)>(_a[12]))); break;
        case 2: _t->resultReady((*reinterpret_cast< PolarAlignmentResult(*)>(_a[1]))); break;
        case 3: _t->errorOccurred((*reinterpret_cast< QString(*)>(_a[1]))); break;
        case 4: _t->onStateTimerTimeout(); break;
        case 5: _t->onCaptureAndAnalysisTimerTimeout(); break;
        case 6: _t->onMovementTimerTimeout(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (PolarAlignment::*)(PolarAlignmentState , QString , int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PolarAlignment::stateChanged)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (PolarAlignment::*)(double , double , double , double , double , double , double , double , double , double , QString , QString );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PolarAlignment::adjustmentGuideData)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (PolarAlignment::*)(PolarAlignmentResult );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PolarAlignment::resultReady)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (PolarAlignment::*)(QString );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PolarAlignment::errorOccurred)) {
                *result = 3;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject PolarAlignment::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_PolarAlignment.data,
    qt_meta_data_PolarAlignment,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *PolarAlignment::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *PolarAlignment::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_PolarAlignment.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int PolarAlignment::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 7)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 7;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 7)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 7;
    }
    return _id;
}

// SIGNAL 0
void PolarAlignment::stateChanged(PolarAlignmentState _t1, QString _t2, int _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void PolarAlignment::adjustmentGuideData(double _t1, double _t2, double _t3, double _t4, double _t5, double _t6, double _t7, double _t8, double _t9, double _t10, QString _t11, QString _t12)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t4))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t5))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t6))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t7))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t8))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t9))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t10))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t11))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t12))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void PolarAlignment::resultReady(PolarAlignmentResult _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void PolarAlignment::errorOccurred(QString _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
