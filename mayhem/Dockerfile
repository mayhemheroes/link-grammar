FROM --platform=linux/amd64 ubuntu:20.04 as builder

RUN apt-get update
RUN DEBIAN_FRONTEND=noninteractive apt-get install -y automake libtool pkg-config build-essential autoconf-archive bison flex clang

COPY . /repo
WORKDIR /repo
RUN ./autogen.sh
RUN ./configure --prefix=/install CPPFLAGS=-DHAVE_LOCALE_T_IN_LOCALE_H=1 CFLAGS=-DHAVE_LOCALE_T_IN_LOCALE_H=1
RUN make -j8 CPPFLAGS=-DHAVE_LOCALE_T_IN_LOCALE_H=1
RUN make install
RUN sed -i 's/String_set \* string_set_create(void);/extern "C" String_set \* string_set_create(void);/' /repo/link-grammar/string-set.h
RUN sed -i 's/const char \* string_set_add/extern "C" const char \* string_set_add/' /repo/link-grammar/string-set.h
RUN sed -i 's/void         string_set_delete/extern "C" void string_set_delete/' /repo/link-grammar/string-set.h
RUN clang++ /repo/mayhem/fuzz_string_set_add.cpp -o /fuzz -fsanitize=fuzzer,address -DHAVE_LOCALE_T_IN_LOCALE_H=1 -I /repo/link-grammar/ -I /repo/ -L /install/lib/ -l:liblink-grammar.a 

FROM ubuntu:20.04 as package
COPY --from=builder /install /install
COPY --from=builder /fuzz /fuzz_string_set_add
