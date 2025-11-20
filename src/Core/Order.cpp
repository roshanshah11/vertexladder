#include "orderbook/Core/Order.hpp"
#include "orderbook/Utilities/MemoryManager.hpp"

namespace orderbook {

void* Order::operator new(size_t size) {
    return MemoryManager::getInstance().allocateAligned(size, 64);
}

void Order::operator delete(void* ptr) {
    MemoryManager::getInstance().deallocateAligned(ptr, sizeof(Order));
}

}
