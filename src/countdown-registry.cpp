#include "countdown-registry.h"

#include <mutex>

namespace cdreg {
namespace {

struct Item {
	void *handle = nullptr;
	ActionFn action = nullptr;
	Entry entry;
};

struct Table {
	std::mutex lock;
	/* 实例数量最多几十个，线性扫描比维护索引更省事，而且天然保持登记顺序 */
	std::vector<Item> items;
	uint64_t next_id = 1;
};

/* 函数内静态对象：避免插件卸载时的静态析构顺序问题 */
Table &table()
{
	static Table t;
	return t;
}

Item *find(Table &t, void *handle)
{
	for (Item &item : t.items) {
		if (item.handle == handle)
			return &item;
	}
	return nullptr;
}

} /* namespace */

uint64_t add(void *handle, ActionFn action)
{
	if (!handle || !action)
		return 0;

	Table &t = table();
	std::lock_guard<std::mutex> guard(t.lock);

	if (Item *existing = find(t, handle))
		return existing->entry.id;

	Item item;
	item.handle = handle;
	item.action = action;
	item.entry.id = t.next_id++;
	t.items.push_back(std::move(item));
	return t.items.back().entry.id;
}

void remove(void *handle)
{
	Table &t = table();
	std::lock_guard<std::mutex> guard(t.lock);

	for (size_t i = 0; i < t.items.size(); ++i) {
		if (t.items[i].handle == handle) {
			t.items.erase(t.items.begin() + (ptrdiff_t)i);
			return;
		}
	}
}

void refresh(void *handle, const char *name, const Status &status)
{
	Table &t = table();
	std::lock_guard<std::mutex> guard(t.lock);

	Item *item = find(t, handle);
	if (!item)
		return;

	Entry &e = item->entry;
	if (name && *name)
		e.name = name;
	e.status = status;
}

std::vector<Entry> snapshot()
{
	Table &t = table();
	std::lock_guard<std::mutex> guard(t.lock);

	std::vector<Entry> out;
	out.reserve(t.items.size());
	for (const Item &item : t.items)
		out.push_back(item.entry);
	return out;
}

bool perform(uint64_t id, int action)
{
	if (id == 0)
		return false;

	Table &t = table();
	std::lock_guard<std::mutex> guard(t.lock);

	for (Item &item : t.items) {
		if (item.entry.id == id) {
			item.action(item.handle, action);
			return true;
		}
	}
	return false;
}

} /* namespace cdreg */
