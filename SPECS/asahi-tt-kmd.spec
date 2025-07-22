Name: tt-kmd
Version: 2.1.0
Release: %autorelease
Summary: Tenstorrent AI Kernel-Mode Driver

License: GPL-2.0
URL: https://github.com/tenstorrent/tt-kmd
Source0: %{URL}/archive/refs/tags/ttkmd-%{version}.tar.gz

BuildArch: noarch
BuildRequires: kernel-16k-devel dkms
Requires: kernel-16k-devel dkms

%description
This package provides the %{name} kernel module via DKMS.

%prep
%setup -q -n %{name}-ttkmd-%{version}

%build
# No build needed; DKMS handles compilation

%install
mkdir -p %{buildroot}/usr/src/%{name}-%{version}
cp -a * %{buildroot}/usr/src/%{name}-%{version}


%files
%defattr(-,root,root,-)
/usr/src/%{name}-%{version}

%changelog
%autochangelog

