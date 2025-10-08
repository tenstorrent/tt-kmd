Name:           tenstorrent-dkms
Version:        0.0.0
Release:        1%{?dist}
Summary:        Tenstorrent kernel mode driver (DKMS)

License:        MIT
URL:            https://github.com/tenstorrent/tt-kmd
Source0:        %{name}-%{version}.tar.gz

BuildArch:      noarch
Requires:       dkms
Requires:       kernel-devel

%description
This package provides the Tenstorrent kernel module as DKMS source.

%prep
%setup -q -n tenstorrent-%{version}

%build
# No build required - DKMS will build the module

%install
mkdir -p %{buildroot}/usr/src/tenstorrent-%{version}

# Install source files, excluding build artifacts
cp -r contrib docs tools test %{buildroot}/usr/src/tenstorrent-%{version}/

# Install individual files
install -m 644 AKMBUILD %{buildroot}/usr/src/tenstorrent-%{version}/
install -m 644 Makefile %{buildroot}/usr/src/tenstorrent-%{version}/
install -m 644 LICENSE %{buildroot}/usr/src/tenstorrent-%{version}/
install -m 644 LICENSE_understanding.txt %{buildroot}/usr/src/tenstorrent-%{version}/
install -m 644 README.md %{buildroot}/usr/src/tenstorrent-%{version}/
install -m 644 SUMMARY.md %{buildroot}/usr/src/tenstorrent-%{version}/
install -m 644 dkms.conf %{buildroot}/usr/src/tenstorrent-%{version}/
install -m 755 dkms-post-install %{buildroot}/usr/src/tenstorrent-%{version}/
install -m 644 modprobe.d-tenstorrent.conf %{buildroot}/usr/src/tenstorrent-%{version}/
install -m 644 udev-50-tenstorrent.rules %{buildroot}/usr/src/tenstorrent-%{version}/

# Install all C source and header files
install -m 644 *.c %{buildroot}/usr/src/tenstorrent-%{version}/
install -m 644 *.h %{buildroot}/usr/src/tenstorrent-%{version}/

%post
# Add module to DKMS
dkms add -m tenstorrent -v %{version} || :
dkms build -m tenstorrent -v %{version} || :
dkms install -m tenstorrent -v %{version} || :

%preun
# Remove module from DKMS before uninstall
dkms remove -m tenstorrent -v %{version} --all || :

%files
/usr/src/tenstorrent-%{version}

%changelog
* Wed Oct 08 2025 Tenstorrent Releases <releases@tenstorrent.com> - 0.0.0-1
- Initial RPM package
