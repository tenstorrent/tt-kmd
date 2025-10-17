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

# Build and install for all installed kernels that have kernel headers
for kernel in /lib/modules/*/build; do
    if [ -d "$kernel" ]; then
        kernel_ver=$(basename $(dirname "$kernel"))
        dkms build -m tenstorrent -v %{version} -k "$kernel_ver" 2>/dev/null || :
        dkms install -m tenstorrent -v %{version} -k "$kernel_ver" 2>/dev/null || :
    fi
done

# Try to load the module for the running kernel if it was built successfully
RUNNING_KERNEL=$(uname -r)
if [ -f /lib/modules/$RUNNING_KERNEL/extra/tenstorrent.ko ] || \
   [ -f /lib/modules/$RUNNING_KERNEL/kernel/extra/tenstorrent.ko ] || \
   [ -f /lib/modules/$RUNNING_KERNEL/updates/dkms/tenstorrent.ko ]; then
    modprobe tenstorrent 2>/dev/null || :
fi

%preun
# Remove module from DKMS before uninstall
dkms remove -m tenstorrent -v %{version} --all || :

%files
/usr/src/tenstorrent-%{version}

%changelog
* Wed Oct 08 2025 Tenstorrent Releases <releases@tenstorrent.com> - 0.0.0-1
- Initial RPM package
