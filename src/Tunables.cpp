#include "Tunables.h"
#include <string.h>
#include <math.h>

namespace Tunables {

// Constant-initialised (zero) before any constructor runs, so registering from
// other files' static objects is safe whatever their order.
static Entry* s_head = nullptr;

void add(Entry* e) noexcept {
    e->next = s_head;
    s_head = e;
}

void toJson(JsonDocument& doc) {
    for (Entry* e = s_head; e; e = e->next) {
        if (!doc[e->name].isNull()) {
            doc["duplicates"].add(e->name);
            continue;
        }
        if (e->text) doc[e->name] = e->text;
        else doc[e->name] = e->number;
    }
}

double number(const char* name) {
    for (Entry* e = s_head; e; e = e->next)
        if (!e->text && strcmp(e->name, name) == 0) return e->number;
    return NAN;
}

}  // namespace Tunables
