.. -*- rst -*-

.. highlightlang:: none

CentOS
======

This section describes how to install Groonga related RPM packages on
CentOS. You can install them by ``yum``.

We distribute both 32-bit and 64-bit packages but we strongly
recommend a 64-bit package for server. You should use a 32-bit package
just only for tests or development. You will encounter an out of
memory error with a 32-bit package even if you just process medium
size data.

CentOS 5
--------

Install::

  % sudo rpm -ivh http://packages.groonga.org/centos/groonga-release-1.1.0-1.noarch.rpm
  % sudo yum makecache
  % sudo yum install -y groonga

.. include:: server-use.inc

If you want to use `MeCab <http://mecab.sourceforge.net/>`_ as a
tokenizer, install groonga-tokenizer-mecab package.

Install groonga-tokenizer-mecab package::

  % sudo yum install -y groonga-tokenizer-mecab

There is a package that provides `Munin
<http://munin-monitoring.org/>`_ plugins. If you want to monitor
Groonga status by Munin, install groonga-munin-plugins package.

.. note::

   Groonga-munin-plugins package requires munin-node package that
   isn't included in the official CentOS repository. You need to
   enable `Repoforge (RPMforge) <http://repoforge.org/>`_ repository
   or `EPEL <http://fedoraproject.org/wiki/EPEL>`_ repository to
   install it by ``yum``.

   Enable Repoforge (RPMforge) repository on i386 environment::

     % wget http://pkgs.repoforge.org/rpmforge-release/rpmforge-release-0.5.3-1.el5.rf.i386.rpm
     % sudo rpm -ivh rpmforge-release-0.5.2-2.el5.rf.i386.rpm

   Enable Repoforge (RPMforge) repository on x86_64 environment::

     % wget http://pkgs.repoforge.org/rpmforge-release/rpmforge-release-0.5.3-1.el5.rf.x86_64.rpm
     % sudo rpm -ivh rpmforge-release-0.5.2-2.el5.rf.x86_64.rpm

   Enable EPEL repository on any environment::

     % wget http://download.fedoraproject.org/pub/epel/5/i386/epel-release-5-4.noarch.rpm
     % sudo rpm -ivh epel-release-5-4.noarch.rpm

Install groonga-munin-plugins package::

  % sudo yum install -y groonga-munin-plugins

There is a package that provides MySQL compatible normalizer as
a Groonga plugin.
If you want to use that one, install groonga-normalizer-mysql package.

Install groonga-normalizer-mysql package::

  % sudo yum install -y groonga-normalizer-mysql

CentOS 6
--------

Install::

  % sudo rpm -ivh http://packages.groonga.org/centos/groonga-release-1.1.0-1.noarch.rpm
  % sudo yum makecache
  % sudo yum install -y groonga

.. include:: server-use.inc

If you want to use `MeCab <http://mecab.sourceforge.net/>`_ as a
tokenizer, install groonga-tokenizer-mecab package.

Install groonga-tokenizer-mecab package::

  % sudo yum install -y groonga-tokenizer-mecab

There is a package that provides `Munin
<http://munin-monitoring.org/>`_ plugins. If you want to monitor
Groonga status by Munin, install groonga-munin-plugins package.

.. note::

   Groonga-munin-plugins package requires munin-node package that
   isn't included in the official CentOS repository. You need to
   enable `EPEL <http://fedoraproject.org/wiki/EPEL>`_ repository to
   install it by ``yum``.

   Enable EPEL repository on any environment::

     % sudo rpm -ivh http://download.fedoraproject.org/pub/epel/6/i386/epel-release-6-8.noarch.rpm

Install groonga-munin-plugins package::

  % sudo yum install -y groonga-munin-plugins

There is a package that provides MySQL compatible normalizer as
a Groonga plugin.
If you want to use that one, install groonga-normalizer-mysql package.

Install groonga-normalizer-mysql package::

  % sudo yum install -y groonga-normalizer-mysql

CentOS 7
--------

Install::

  % sudo rpm -ivh http://packages.groonga.org/centos/groonga-release-1.1.0-1.noarch.rpm
  % sudo yum makecache
  % sudo yum install -y groonga

.. include:: server-use.inc

If you want to use `MeCab <http://mecab.sourceforge.net/>`_ as a
tokenizer, install groonga-tokenizer-mecab package.

Install groonga-tokenizer-mecab package::

  % sudo yum install -y groonga-tokenizer-mecab

There is a package that provides `Munin
<http://munin-monitoring.org/>`_ plugins. If you want to monitor
Groonga status by Munin, install groonga-munin-plugins package.

.. note::

   Groonga-munin-plugins package requires munin-node package that
   isn't included in the official CentOS repository. You need to
   enable `EPEL <http://fedoraproject.org/wiki/EPEL>`_ repository to
   install it by ``yum``.

   Enable EPEL repository::

     % sudo yum install -y epel-release

Install groonga-munin-plugins package::

  % sudo yum install -y groonga-munin-plugins

There is a package that provides MySQL compatible normalizer as
a Groonga plugin.
If you want to use that one, install groonga-normalizer-mysql package.

Install groonga-normalizer-mysql package::

  % sudo yum install -y groonga-normalizer-mysql

Build from source
-----------------

Install required packages to build Groonga::

  % sudo yum install -y wget tar gcc-c++ make mecab-devel

Download source::

  % wget http://packages.groonga.org/source/groonga/groonga-6.0.0.tar.gz
  % tar xvzf groonga-6.0.0.tar.gz
  % cd groonga-6.0.0

Configure (see :ref:`source-configure` about ``configure`` options)::

  % ./configure

Build::

  % make -j$(grep '^processor' /proc/cpuinfo | wc -l)

Install::

  % sudo make install
