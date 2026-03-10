#include "SdkSerialExecutor.h"

#include <QDebug>

SdkSerialExecutor::SdkSerialExecutor(const QString& threadName)
{
    m_worker = new QObject();
    m_worker->moveToThread(&m_thread);

    m_thread.setObjectName(threadName);
    QObject::connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    m_thread.start();
}

SdkSerialExecutor::~SdkSerialExecutor()
{
    if (m_thread.isRunning())
    {
        m_thread.quit();
        m_thread.wait();
    }
    m_worker = nullptr;
}

bool SdkSerialExecutor::isRunning() const
{
    return m_thread.isRunning();
}

void SdkSerialExecutor::post(std::function<void()> fn)
{
    if (!m_worker || !m_thread.isRunning())
        return;

    // Qt5 支持把 functor 投递到目标线程执行（QueuedConnection）
    QMetaObject::invokeMethod(
        m_worker,
        [task = std::move(fn)]() mutable {
            task();
        },
        Qt::QueuedConnection);
}


