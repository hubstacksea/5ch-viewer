# 5ch Viewer
#   Linux native:         make
#   Windows (cross, mingw): make win        (needs static wx build; set WXMINGW=/path/to/prefix)
#   Windows (MSYS2 shell):  make            (wx-config from MSYS2 mingw package)
WXMINGW ?= $(abspath $(CURDIR)/../wx-mingw-3.2.6-full)
SRCS := src/main.cpp src/json.h src/textutil.h
VERSION ?= 1.4.0
CXXFLAGS ?= -O2
# override: rpmbuild passes CXXFLAGS on the command line, which would otherwise drop these
override CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-unused-parameter -DAPP_VERSION='"$(VERSION)"'
# rebuild when VERSION changes (it is compiled into the binary)
$(shell echo "$(VERSION)" | cmp -s - .version || echo "$(VERSION)" > .version)

all: 5chviewer

5chviewer: $(SRCS) Makefile .version
	$(CXX) $(CXXFLAGS) src/main.cpp -o $@ `wx-config --cxxflags --libs core,base,net,html,aui`

win: 5chviewer.exe

5chviewer.exe: $(SRCS) Makefile .version app.rc packaging/5chviewer.ico
	x86_64-w64-mingw32-windres -I$(WXMINGW)/include/wx-3.2 -I$(WXMINGW)/lib/wx/include/x86_64-w64-mingw32-msw-unicode-static-3.2 -DwxUSE_RC_MANIFEST -DwxUSE_DPI_AWARE_MANIFEST=2 app.rc -O coff -o app.o
	x86_64-w64-mingw32-g++ $(CXXFLAGS) src/main.cpp app.o -o $@ \
	  `$(WXMINGW)/bin/wx-config --prefix=$(WXMINGW) --cxxflags --libs core,base,net,html,aui` \
	  -static -static-libgcc -static-libstdc++ -s

clean:
	rm -rf 5chviewer 5chviewer.exe app.o build dist .version
PREFIX ?= /usr
install: 5chviewer
	install -Dm755 5chviewer $(DESTDIR)$(PREFIX)/bin/5chviewer
	install -Dm644 packaging/5chviewer.desktop $(DESTDIR)$(PREFIX)/share/applications/5chviewer.desktop
	install -Dm644 packaging/5chviewer.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/5chviewer.svg
	install -Dm644 README.md $(DESTDIR)$(PREFIX)/share/doc/5chviewer/README.md
	install -Dm644 LICENSE $(DESTDIR)$(PREFIX)/share/doc/5chviewer/LICENSE
	install -Dm644 packaging/THIRD_PARTY_NOTICES.txt $(DESTDIR)$(PREFIX)/share/doc/5chviewer/THIRD_PARTY_NOTICES.txt

# Windows installer (NSIS). NSIS_DIR: directory containing Stubs/ Plugins/ Include/ (default: system install)
MAKENSIS ?= makensis
installer: 5chviewer.exe
	mkdir -p dist
	$(if $(NSIS_DIR),NSISDIR=$(NSIS_DIR)) $(MAKENSIS) -V2 -DVERSION=$(VERSION) -DEXE=$(CURDIR)/5chviewer.exe \
	  -DOUTFILE=$(CURDIR)/dist/5chviewer-$(VERSION)-setup.exe packaging/5chviewer.nsi

deb: 5chviewer
	packaging/build-deb.sh $(VERSION)

rpm:
	packaging/build-rpm.sh $(VERSION)

.PHONY: all win clean install deb rpm installer
