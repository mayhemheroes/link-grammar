#include <stdint.h>
#include <stdio.h>
#include <climits>

#include <fuzzer/FuzzedDataProvider.h>
#include "string-set.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FuzzedDataProvider provider(data, size);
    
    std::string str = provider.ConsumeRandomLengthString(1000);
    const char* cstr = str.c_str();

    String_set_s* ss = string_set_create();
    string_set_add(cstr, ss);
    string_set_delete(ss);

    return 0;
}