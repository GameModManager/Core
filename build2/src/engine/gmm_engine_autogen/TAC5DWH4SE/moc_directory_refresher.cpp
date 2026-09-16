/****************************************************************************
** Meta object code from reading C++ file 'directory_refresher.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../../src/engine/core/directory_refresher.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'directory_refresher.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.11.2. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN6engine17PlaceholderWorkerE_t {};
} // unnamed namespace

template <> constexpr inline auto engine::PlaceholderWorker::qt_create_metaobjectdata<qt_meta_tag_ZN6engine17PlaceholderWorkerE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "engine::PlaceholderWorker",
        "finished",
        "",
        "target",
        "success"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'finished'
        QtMocHelpers::SignalData<void(int, bool)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 3 }, { QMetaType::Bool, 4 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<PlaceholderWorker, qt_meta_tag_ZN6engine17PlaceholderWorkerE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject engine::PlaceholderWorker::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN6engine17PlaceholderWorkerE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN6engine17PlaceholderWorkerE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN6engine17PlaceholderWorkerE_t>.metaTypes,
    nullptr
} };

void engine::PlaceholderWorker::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<PlaceholderWorker *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->finished((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<bool>>(_a[2]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (PlaceholderWorker::*)(int , bool )>(_a, &PlaceholderWorker::finished, 0))
            return;
    }
}

const QMetaObject *engine::PlaceholderWorker::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *engine::PlaceholderWorker::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN6engine17PlaceholderWorkerE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int engine::PlaceholderWorker::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 1)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 1;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 1)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 1;
    }
    return _id;
}

// SIGNAL 0
void engine::PlaceholderWorker::finished(int _t1, bool _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2);
}
namespace {
struct qt_meta_tag_ZN6engine18DirectoryRefresherE_t {};
} // unnamed namespace

template <> constexpr inline auto engine::DirectoryRefresher::qt_create_metaobjectdata<qt_meta_tag_ZN6engine18DirectoryRefresherE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "engine::DirectoryRefresher",
        "refresh_started",
        "",
        "RefreshTarget",
        "target",
        "refresh_finished",
        "success",
        "all_refreshes_finished",
        "progress",
        "percentage",
        "status_text"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'refresh_started'
        QtMocHelpers::SignalData<void(enum RefreshTarget)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 },
        }}),
        // Signal 'refresh_finished'
        QtMocHelpers::SignalData<void(enum RefreshTarget, bool)>(5, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 }, { QMetaType::Bool, 6 },
        }}),
        // Signal 'all_refreshes_finished'
        QtMocHelpers::SignalData<void()>(7, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'progress'
        QtMocHelpers::SignalData<void(enum RefreshTarget, int, const QString &)>(8, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 }, { QMetaType::Int, 9 }, { QMetaType::QString, 10 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<DirectoryRefresher, qt_meta_tag_ZN6engine18DirectoryRefresherE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject engine::DirectoryRefresher::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN6engine18DirectoryRefresherE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN6engine18DirectoryRefresherE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN6engine18DirectoryRefresherE_t>.metaTypes,
    nullptr
} };

void engine::DirectoryRefresher::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<DirectoryRefresher *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->refresh_started((*reinterpret_cast<std::add_pointer_t<enum RefreshTarget>>(_a[1]))); break;
        case 1: _t->refresh_finished((*reinterpret_cast<std::add_pointer_t<enum RefreshTarget>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<bool>>(_a[2]))); break;
        case 2: _t->all_refreshes_finished(); break;
        case 3: _t->progress((*reinterpret_cast<std::add_pointer_t<enum RefreshTarget>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[3]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (DirectoryRefresher::*)(RefreshTarget )>(_a, &DirectoryRefresher::refresh_started, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (DirectoryRefresher::*)(RefreshTarget , bool )>(_a, &DirectoryRefresher::refresh_finished, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (DirectoryRefresher::*)()>(_a, &DirectoryRefresher::all_refreshes_finished, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (DirectoryRefresher::*)(RefreshTarget , int , const QString & )>(_a, &DirectoryRefresher::progress, 3))
            return;
    }
}

const QMetaObject *engine::DirectoryRefresher::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *engine::DirectoryRefresher::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN6engine18DirectoryRefresherE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int engine::DirectoryRefresher::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 4)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 4;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 4)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 4;
    }
    return _id;
}

// SIGNAL 0
void engine::DirectoryRefresher::refresh_started(RefreshTarget _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void engine::DirectoryRefresher::refresh_finished(RefreshTarget _t1, bool _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1, _t2);
}

// SIGNAL 2
void engine::DirectoryRefresher::all_refreshes_finished()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void engine::DirectoryRefresher::progress(RefreshTarget _t1, int _t2, const QString & _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1, _t2, _t3);
}
QT_WARNING_POP
