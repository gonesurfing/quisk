from __future__ import print_function

from setuptools import setup, Extension
import sys
import os
import struct

if sys.platform == 'darwin':
  cmd='rm -f mac.quisk'; os.system(cmd)
  if sys.version_info.major == 2:
    cmd='echo "#!/usr/bin/env python2" > mac.quisk';    os.system(cmd)
    cmd='grep -v /usr/bin/python quisk >> mac.quisk';   os.system(cmd)
    cmd='chmod 755 mac.quisk';                          os.system(cmd)
  if sys.version_info.major == 3:
    cmd='echo "#!/usr/bin/env python3" > mac.quisk';    os.system(cmd)
    cmd='grep -v /usr/bin/python quisk >> mac.quisk';   os.system(cmd)
    cmd='chmod 755 mac.quisk';                          os.system(cmd)

# You must define the version here.  A title string including
# the version will be written to __init__.py and read by quisk.py.

Version = '4.1.73'

fp = open("__init__.py", "w")	# write title string
fp.write("#Quisk version %s\n" % Version)
fp.close()

is_64bit = struct.calcsize("P") == 8

have_portaudio  = False
have_pulseaudio = False
have_wasapi     = False
have_directx    = False
have_alsa       = False
mac_usr_local   = False
have_ftd2xx     = False
myincdir        = "/usr/include"
mylibdir        = "/usr/lib"

if sys.platform == "win32":
  have_wasapi = True
  have_directx = True
elif sys.platform == "darwin" and os.path.exists('/opt/local/include'):
  #
  # MacOS, additional packages in /opt/local (default for MacPorts)
  #
  myincdir="/opt/local/include"
  mylibdir="/opt/local/lib"
  try:
    import wx
  except ImportError:
    print ("Please install the package python-wxgtk3.0 or later")
  if not os.path.isfile("/opt/local/include/fftw3.h"):
    print ("Please install the FFTW3 package")
  if os.path.isfile("/opt/local/include/portaudio.h"):
    have_portaudio = True
  if os.path.isdir("/opt/local/include/pulse"):
    have_pulseaudio = True
  if os.path.isfile("/opt/local/include/ftd2xx.h"):
    have_ftd2xx = True
elif sys.platform == "darwin" and os.path.exists('/usr/local/include'):
  #
  # MacOS, additional packages in /usr/local (default for Homebrew)
  #
  myincdir="/usr/local/include"
  mylibdir="/usr/local/lib"
  print("MacOS with extension in /usr/local")
  try:
    import wx
  except ImportError:
    print ("Please install the package python-wxgtk3.0 or later")
  if not os.path.isfile("/usr/local/include/fftw3.h"):
    print ("Please install the FFTW3 package")
  if os.path.isfile("/usr/local/include/portaudio.h"):
    have_portaudio = True
  if os.path.isdir("/usr/local/include/pulse"):
    have_pulseaudio = True
  if os.path.isfile("/usr/local/include/ftd2xx.h"):
    have_ftd2xx = True
else:
  try:
    import wx
  except ImportError:
    print ("Please install the package python-wxgtk3.0 or later")
  if not os.path.isfile("/usr/include/fftw3.h"):
    print ("Please install the package libfftw3-dev")
  if not os.path.isdir("/usr/include/alsa"):
    print ("Please install the package libasound2-dev")
  if os.path.isfile("/usr/include/portaudio.h"):
    have_portaudio = True
  if os.path.isdir("/usr/include/pulse"):
    have_pulseaudio = True
  if os.path.isfile("/usr/include/ftd2xx.h"):
    have_ftd2xx = True
  have_alsa = True

libraries = ['fftw3', 'm']
sources = ['quisk.c', 'sound.c', 
	'is_key_down.c', 'microphone.c', 'utility.c',
	'filter.c', 'extdemod.c', 'freedv.c']

define_macros = []

if have_wasapi:
	define_macros.append(("QUISK_HAVE_WASAPI", None))

if have_wasapi:
	define_macros.append(("QUISK_HAVE_DIRECTX", None))

if have_alsa:
	libraries.append('asound')
	sources.append('sound_alsa.c')
	define_macros.append(("QUISK_HAVE_ALSA", None))

if have_portaudio:
	libraries.append('portaudio')
	sources.append('sound_portaudio.c')
	define_macros.append(("QUISK_HAVE_PORTAUDIO", None))

if have_pulseaudio:
	libraries.append('pulse')
	sources.append('sound_pulseaudio.c')
	define_macros.append(("QUISK_HAVE_PULSEAUDIO", None))

#
# Linux modules
#
module1 = Extension ('quisk._quisk',
	libraries = libraries,
	sources = sources,
	define_macros = define_macros,
	)

module2 = Extension ('quisk.sdriqpkg.sdriq',
	libraries = ['m'],
	sources = ['import_quisk_api.c', 'sdriqpkg/sdriq.c'],
	include_dirs = ['.'],
	)

# Afedri hardware support added by Alex, Alex@gmail.com
module3 = Extension ('quisk.afedrinet.afedrinet_io',
	libraries = ['m'],
	sources = ['import_quisk_api.c', 'is_key_down.c', 'afedrinet/afedrinet_io.c'],
	include_dirs = ['.'],
	)

#
# Windows modules
#
modulew1 = Extension ('quisk._quisk',
	include_dirs = ['../fftw3'],
	#include_dirs = ['../fftw3', 'C:/Program Files (x86)/Microsoft DirectX SDK (February 2010)/Include',
	#     'C:/Program Files/Microsoft DirectX SDK (February 2010)/Include',],
	library_dirs = ['../fftw3'],
	libraries = ['fftw3-3', 'WS2_32', 'Dxguid', 'Dsound', 'iphlpapi'],
	sources = ['quisk.c', 'sound.c', 'sound_directx.c',
		'is_key_down.c', 'microphone.c', 'utility.c',
		'filter.c', 'extdemod.c', 'freedv.c', 'sound_wasapi.c'],
	)

modulew2 = Extension ('quisk.sdriqpkg.sdriq',
	libraries = [':ftd2xx.lib'],
	library_dirs = ['../ftdi/i386'],
	sources = ['import_quisk_api.c', 'sdriqpkg/sdriq.c'],
	include_dirs = ['.', '../ftdi'],
	#extra_link_args = ['--enable-auto-import'],
	)

# Afedri hardware support added by Alex, Alex@gmail.com
modulew3 = Extension ('quisk.afedrinet.afedrinet_io',
	libraries = ['WS2_32'],
	sources = ['import_quisk_api.c', 'is_key_down.c', 'afedrinet/afedrinet_io.c'],
	include_dirs = ['.'],
	)

modulew4 = Extension ('quisk.soapypkg.soapy',
	sources = ['import_quisk_api.c', 'soapypkg/soapy.c'],
	include_dirs = [".", "c:/Program Files/PothosSDR/include"],
	libraries = ['WS2_32', 'SoapySDR'],
	)

# Changes for MacOS support thanks to Mario, DL3LSM.
# Changes by Jim, N1ADJ.
modulem1 = Extension ('quisk._quisk',
	include_dirs = ['.', myincdir],
	library_dirs = ['.', mylibdir],
	libraries = libraries,
	sources = sources,
	define_macros = define_macros,
	)

modulem2 = Extension ('quisk.sdriqpkg.sdriq',
	libraries = ['m', 'ftd2xx'],
	sources = ['import_quisk_api.c', 'sdriqpkg/sdriq.c'],
	include_dirs = ['.', '..', myincdir],
	library_dirs = ['.', mylibdir],
	)


if sys.platform == "win32":
  Modules = [modulew1, modulew2, modulew3]
  if is_64bit:
    Modules.append(modulew4)
  requires = ['wxPython', 'pyusb']
elif sys.platform == "darwin":
  if have_ftd2xx:
    Modules = [modulem1,modulem2]
  else:
    Modules = [modulem1]
  requires = ['wxPython', 'pyusb']
else:
  if have_ftd2xx:
    Modules = [module1, module2, module3]
  else:
    Modules = [module1, module3]
  requires = []

setup	(name = 'quisk',
	version = Version,
	description = 'QUISK is a Software Defined Radio (SDR) transceiver that can control various radio hardware.',
	long_description = """QUISK is a Software Defined Radio (SDR) transceiver.  
You supply radio hardware that converts signals at the antenna to complex (I/Q) data at an
intermediate frequency (IF). Data can come from a sound card, Ethernet or USB. Quisk then filters and
demodulates the data and sends the audio to your speakers or headphones. For transmit, Quisk takes
the microphone signal, converts it to I/Q data and sends it to the hardware.

Quisk can be used with SoftRock, Hermes Lite 2, HiQSDR, Odyssey and many radios that use the Hermes protocol.
Quisk can connect to digital programs like Fldigi and WSJT-X. Quisk can be connected to other software like
N1MM+ and software that uses Hamlib.
""",
	author = 'James C. Ahlstrom',
	author_email = 'jahlstr@gmail.com',
	url = 'http://james.ahlstrom.name/quisk/',
	packages = ['quisk', 'quisk.sdriqpkg', 'quisk.n2adr', 'quisk.softrock', 'quisk.freedvpkg',
		'quisk.hermes', 'quisk.hiqsdr', 'quisk.afedrinet', 'quisk.soapypkg', 'quisk.sdrmicronpkg', 'quisk.perseuspkg'],
	package_dir =  {'quisk' : '.'},
	package_data = {'' : ['*.txt', '*.html', '*.so', '*.dll']},
	entry_points = {'gui_scripts' : ['quisk = quisk.quisk:main', 'quisk_vna = quisk.quisk_vna:main']},
	ext_modules = Modules,
	install_requires = requires,
	provides = ['quisk'],
	classifiers = [
		'Development Status :: 6 - Mature',
		'Environment :: X11 Applications',
		'Environment :: Win32 (MS Windows)',
		'Intended Audience :: End Users/Desktop',
		'License :: OSI Approved :: GNU General Public License (GPL)',
		'Natural Language :: English',
		'Operating System :: POSIX :: Linux',
		'Operating System :: Microsoft :: Windows',
		'Programming Language :: Python :: 2.7',
		'Programming Language :: Python :: 3',
		'Programming Language :: C',
		'Topic :: Communications :: Ham Radio',
	],
)


