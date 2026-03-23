#pragma once

#include <QObject>
#include <QThread>
#include <QMetaObject>

#include <functional>
#include <future>
#include <type_traits>
#include <memory>

/**
 * 串行执行器：把任务按顺序投递到一个专用 QThread 的事件循环中执行。
 *
 * 目的：
 * - 避免在主线程调用阻塞式 SDK（USB/串口/等待曝光/读帧）
 * - 保证同一时刻只有一个 SDK 任务在跑（很多厂商 SDK 非线程安全）
 */
class SdkSerialExecutor
{
public:
    explicit SdkSerialExecutor(const QString& threadName = QStringLiteral("SdkSerialExecutor"));
    ~SdkSerialExecutor();

    SdkSerialExecutor(const SdkSerialExecutor&) = delete;
    SdkSerialExecutor& operator=(const SdkSerialExecutor&) = delete;

    bool isRunning() const;

    // 投递一个任务到 SDK 线程（按提交顺序串行执行）
    void post(std::function<void()> fn);

    /**
     * 同步投递任务并等待结果返回。
     *
     * 重要：若在 executor 所在线程调用，会直接执行 fn()，避免死锁。
     */
    template <typename R>
    R postAndWait(std::function<R()> fn)
    {
        if (!m_worker || !m_thread.isRunning())
        {
            // 对于不可运行的 executor，返回默认值（调用者通常会把其视为失败）
            return R{};
        }

        // 若本身就在 executor 线程，直接执行，避免阻塞造成死锁
        if (QThread::currentThread() == &m_thread)
        {
            return fn();
        }

        auto prom = std::make_shared<std::promise<R>>();
        auto fut = prom->get_future();

        // 注意：post() 接收 std::function<void()>，其 target 必须可拷贝；
        // 因此这里用 shared_ptr 包裹 promise，避免 move-only 捕获导致编译失败。
        post([prom, task = std::move(fn)]() mutable {
            try
            {
                prom->set_value(task());
            }
            catch (...)
            {
                prom->set_exception(std::current_exception());
            }
        });

        return fut.get();
    }

    // void 特化：同步等待，但不返回值
    void postAndWait(std::function<void()> fn)
    {
        if (!m_worker || !m_thread.isRunning())
            return;
        if (QThread::currentThread() == &m_thread)
        {
            fn();
            return;
        }

        auto prom = std::make_shared<std::promise<void>>();
        auto fut = prom->get_future();
        post([prom, task = std::move(fn)]() mutable {
            try
            {
                task();
                prom->set_value();
            }
            catch (...)
            {
                prom->set_exception(std::current_exception());
            }
        });
        fut.get();
    }

private:
    QThread  m_thread;
    QObject* m_worker{nullptr};
};


