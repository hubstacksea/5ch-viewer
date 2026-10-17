%global debug_package %{nil}
%global _build_id_links none

Name:           5chviewer
Version:        1.4.1
Release:        1%{?dist}
Summary:        Tabbed 5ch.io viewer (wxWidgets)
License:        MIT
URL:            https://5ch.io/
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  gcc-c++, make
# Fedora: wxGTK-devel   openSUSE: wxWidgets-3_2-devel (provides wx-config)
BuildRequires:  /usr/bin/wx-config
AutoReqProv:    no
Requires:       libwx_gtk3u_core-3.2.so.0()(64bit)
Requires:       libwx_gtk3u_html-3.2.so.0()(64bit)
Requires:       libwx_gtk3u_aui-3.2.so.0()(64bit)
Requires:       libwx_baseu-3.2.so.0()(64bit)
Requires:       libwx_baseu_net-3.2.so.0()(64bit)
Requires:       libcurl.so.4()(64bit)
Requires:       libstdc++.so.6()(64bit)

%description
Board tree, tabbed thread lists and tabbed thread readers with reply helpers,
new-post markers, NG filters, auto refresh and posting.

%prep
%setup -q

%build
make %{?_smp_mflags} CXXFLAGS="%{optflags}" VERSION=%{version}

%install
make install DESTDIR=%{buildroot} PREFIX=%{_prefix}
strip --strip-unneeded %{buildroot}%{_bindir}/5chviewer

%files
%{_bindir}/5chviewer
%{_datadir}/applications/5chviewer.desktop
%{_datadir}/icons/hicolor/scalable/apps/5chviewer.svg
%doc %{_docdir}/5chviewer/README.md
%license %{_docdir}/5chviewer/THIRD_PARTY_NOTICES.txt
%license %{_docdir}/5chviewer/LICENSE

%changelog
* Sat Oct 17 2026 5ch Viewer maintainers - 1.4.1-1
- Hover popups get a visible frame on all platforms

* Fri Oct 16 2026 5ch Viewer maintainers - 1.4.0-1
- Posting confirmation (terms) is shown and must be accepted by the user
- Third-party licence notices, MIT LICENSE, Help > About
- Auto refresh minimum 60 s; dat-ochi threads read from the official past-log path

* Tue Oct 13 2026 5ch Viewer maintainers - 1.3.0-1
- 5ch thread/board links open as tabs (jump to the post number in the URL)
- Mark own posts and replies to them, notify replies in tabs and status bar

* Thu Oct 08 2026 5ch Viewer maintainers - 1.2.3-1
- Fix "HTML anchor pN does not exist" warning when resuming the reading position

* Thu Oct 08 2026 5ch Viewer maintainers - 1.2.2-1
- Reliable hover popups for small links, popup for all replies of a post

* Thu Oct 08 2026 5ch Viewer maintainers - 1.2.1-1
- Thread list refresh button, board tab context menu, refresh of stale lists

* Thu Oct 08 2026 5ch Viewer maintainers - 1.2.0-1
- Fix long URLs broken by inserted spaces; support ttp:// style links
- Detect dat-ochi threads, keep cached content, stop auto-refreshing them
- Verify differential dat downloads (re-fetch when the dat was rewritten)
- Resume reading position, mark threads new since last session
- Clickable links in popups, auto-switch view to reach out-of-range posts
- Fewer full re-layouts, non-blocking reload after posting, safer shutdown
- Prune dat cache older than 60 days

* Tue Oct 06 2026 5ch Viewer maintainers - 1.1.1-1
- Fix slow wheel scrolling after link clicks
- Fix reply hint text being included in posted message

* Tue Oct 06 2026 5ch Viewer maintainers - 1.1.0-1
- Tabbed boards and threads, HTML reader, fast scrolling
