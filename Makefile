VERSION = $(shell git describe --tags)
PREFIX = /usr
GTK = gtk+-3.0
VTE = vte-2.91
TERMINFO = ${PREFIX}/share/terminfo

CXXFLAGS := -std=c++11 -O3 \
	    -Wall -Wextra -pedantic \
	    -Winit-self \
	    -Wshadow \
	    -Wformat=2 \
	    -Wmissing-declarations \
	    -Wstrict-overflow=5 \
	    -Wcast-align \
	    -Wconversion \
	    -Wunused-macros \
	    -Wwrite-strings \
	    -DNDEBUG \
	    -D_POSIX_C_SOURCE=200809L \
	    -DTERMITE_VERSION=\"${VERSION}\" \
	    ${shell pkg-config --cflags ${GTK} ${VTE}} \
	    ${CXXFLAGS}

ifeq (${CXX}, g++)
	CXXFLAGS += -Wno-missing-field-initializers
endif

ifeq (${CXX}, clang++)
	CXXFLAGS += -Wimplicit-fallthrough
endif

LDFLAGS := -s -Wl,--as-needed ${LDFLAGS}
LDLIBS := ${shell pkg-config --libs ${GTK} ${VTE}}

termise: termise.cc util/maybe.hh
	${CXX} ${CXXFLAGS} ${LDFLAGS} $< ${LDLIBS} -o $@

install: termise termise.desktop termise.terminfo
	mkdir -p ${DESTDIR}${TERMINFO}
	install -Dm755 termise ${DESTDIR}${PREFIX}/bin/termise
	install -Dm644 config ${DESTDIR}/etc/xdg/termise/config
	install -Dm644 termise.desktop ${DESTDIR}${PREFIX}/share/applications/termise.desktop
	tic -x termise.terminfo -o ${DESTDIR}${TERMINFO}

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/termise

clean:
	rm termise

.PHONY: clean install uninstall
