************
Installation
************

Getting pyvsf
=============

At this time, you must build pyvsf from source. 

.. note::

   In the future, we plan to support installation of precompiled binaries from PyPI.

The primary dependency is having a modern C++ compiler installed.
All other python dependencies will be handled by your python package manager.

You currently have 2 options, install from a url or install from a local copy of the pyvsf reposiory.

Install from url (recommended for users)
----------------------------------------

If you simply want to use pyvsf, then the easiest way to install it is to invoke:

.. code-block:: shell-session

   $ python -m pip install -v pyvsf@git+https://github.com/mabruzzo/pyvsf

Install from local copy of repository
-------------------------------------

The other way to install pyvsf is to manually clone the repository and build the package from inside of it (this is recommended for contributors).
You would invoke:

.. code-block:: shell-session

   $ git clone https://github.com/mabruzzo/pyvsf
   $ cd pyvsf
   $ python -m pip install -v .

The above command installs the minimum required dependencies.

It is possible to also install extra dependencies for particular purposes:

* for extra testing, replace ``.`` in the above statement with ``.[dev]``.

* for some of the undocumented functionality, replace ``.`` with ``.[extra-features]``.

* for building docs, replace ``.`` with ``.[docs]``.

Be aware, if you use Z shell (the default shell on modern versions of macOS) you may need to put these snippets inside of single quotes (e.g. ``'.[dev]'`` instead of ``.[dev]``).


OpenMP Support
--------------

The package is automatically compiled with OpenMP support if the compiler supports it.
While most mainstream compilers do support it, on macOS, some extra steps are required (see below).

To check if the package was compiled with OpenMP, you can invoke the following from the command-line (and check if the printed statement mentions OpenMP)

.. code-block:: shell-session

   $ python -m pyvsf

Extra Steps to enable OpenMP for parallelization on macOS
=========================================================

If you want to use OpenMP on macOS, some extra care is required.
Specifically, you need to install a C++ compiler that supports OpenMP.
The default C++ compiler on macOS is an apple-specific version of clang++ that does NOT support OpenMP.

The easiest way to get a different compiler is use homebrew to install a version of g++.

.. note::

   By default, modern versions of macOS provides a file called ``g++`` that simply aliases the default clang++ compiler (they are trying to be helpful, but this can be a little confusing).
   In other words, invoking ``g++`` on the command line ALWAYS redirects to clang++ (whether or not a version of g++ is actually installed). 

   When you use homebrew to install g++, the installed version of g++ will include the version number (e.g. it might create ``g++-10``, ``g++-11``, ``g++-14``, ...).

Now that you have installed a version of g++, (for the sake of argument, we assum its called ``g++-14``), you should invoke

.. tabs::

   .. tab:: Install from URL

      .. code-block:: shell-session

         $ CXX=g++-14 python -m pip install -v pyvsf@git+https://github.com/mabruzzo/pyvsf


   .. tab:: Install from Local Repository

      .. code-block:: shell-session

         $ # from the root of your pyvsf repository
         $ CXX=g++-14 python -m pip install -v .

Tests
=====

To run the tests, you need to install pyvsf from a local copy of the repository and you should be sure to install the extra development requirements (at the time of writing, these dependencies just consists of ``pytest`` and ``scipy``, but that may change in the future).

From the root of your pyvsf repository, you should invoke

.. code-block:: shell-session

   $ python -m pytest
