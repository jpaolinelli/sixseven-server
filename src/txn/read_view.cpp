#include "sixseven/txn/read_view.h"

#include <utility>

namespace sixseven {

namespace {

/// The thread-local read view installed by the executor for the duration of
/// a statement. Thread-local (matching the StatementDeadline pattern) so a
/// shared TableHeap can be scanned by concurrent sessions, each under its own
/// snapshot, without threading a context through every operator constructor.
thread_local const MvccReadView* current_view = nullptr;

} // namespace

const MvccReadView* current_mvcc_read_view() {
    return current_view;
}

MvccReadViewGuard::MvccReadViewGuard(MvccReadView view)
    : view_(std::move(view)), previous_(current_view) {
    current_view = &view_;
}

MvccReadViewGuard::~MvccReadViewGuard() {
    current_view = previous_;
}

} // namespace sixseven
