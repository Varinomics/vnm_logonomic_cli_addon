#pragma once

#include <cstdint>

namespace logonomic {

// The host owns the native-family records. Its callback table must outlive
// each child admitted through it because the child retains the table until
// its record is destroyed.
struct Native_child_custody_callbacks
{
    std::uint32_t version = 1;
    std::uint32_t size = sizeof(Native_child_custody_callbacks);
    int (*reserve)(void** record) = nullptr;
    std::uintptr_t (*job_handle)(void* record) = nullptr;
    bool (*publish_created)(void* record, std::uint32_t process_id, std::uintptr_t native) = nullptr;
    void (*cancel_uncreated)(void* record) = nullptr;
    bool (*observe_reaped)(void* record) = nullptr;
    void (*handoff_wait_authority)(void* record) = nullptr;
    void (*destroy)(void* record) = nullptr;
};

class Native_child_custody
{
public:
    enum class Admission { NOT_REQUIRED, ADMITTED, CLOSED };

    explicit Native_child_custody(const Native_child_custody_callbacks* callbacks)
    :
        m_callbacks(callbacks)
    {
        if (m_callbacks) {
            m_admission = static_cast<Admission>(m_callbacks->reserve(&m_record));
        }
    }

    ~Native_child_custody()
    {
        if (m_record) m_callbacks->destroy(m_record);
    }

    Native_child_custody(const Native_child_custody&) = delete;
    Native_child_custody& operator=(const Native_child_custody&) = delete;

    Admission admission() const { return m_admission; }
    std::uintptr_t job_handle() const { return m_record ? m_callbacks->job_handle(m_record) : 0; }

    bool publish_created(std::uint32_t process_id, std::uintptr_t native)
    {
        return m_record ? m_callbacks->publish_created(m_record, process_id, native)
                        : m_admission == Admission::NOT_REQUIRED;
    }

    void cancel_uncreated()
    {
        if (m_record) m_callbacks->cancel_uncreated(m_record);
    }

    bool observe_reaped()
    {
        return m_record ? m_callbacks->observe_reaped(m_record) : m_admission == Admission::NOT_REQUIRED;
    }

    void handoff_wait_authority()
    {
        if (m_record) m_callbacks->handoff_wait_authority(m_record);
    }

private:
    const Native_child_custody_callbacks* m_callbacks = nullptr;
    void* m_record = nullptr;
    Admission m_admission = Admission::NOT_REQUIRED;
};

} // namespace logonomic
