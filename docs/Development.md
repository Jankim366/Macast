# Macast Development

On windows and Linux, we use **pystray**  to add menubar icon support, and use **pyinstaller** to package application.
We use **rumps** and **py2app** on MacOS which have better performance and smaller package size.


## Development under MacOS

### 1. download mpv

```shell
wget https://laboratory.stolendata.net/~djinn/mpv_osx/mpv-latest.tar.gz
mkdir -p bin && tar --strip-components 2 -C bin -xzvf mpv-latest.tar.gz mpv.app/Contents/MacOS
```

### 2. debug

```shell
pip install -r requirements/darwin.txt
python Macast.py
```

### 3. package

```shell
pip install py2app
pip install setuptools==44.0.0 # try this if you cannot run Macast.app
python setup.py py2app
cp -R bin dist/Macast.app/Contents/Resources/
open dist
```


## Development under Windows

### 1. download mpv

```powershell
$client = new-object System.Net.WebClient
$client.DownloadFile('https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/20261021/mpv-x86_64-20261021-git-e471496c56.7z','mpv.7z')
7z x -obin mpv.7z mpv.exe d3dcompiler_43.dll
```

This build includes mpv's Windows System Media Transport Controls support.
Bundling mpv 0.34 would not expose DLNA playback in the Windows media panel.

### 2. debug

```powershell
pip install -r requirements/common.txt
python Macast.py
```

### 3. package

```powershell
pip install pyinstaller
powershell -ExecutionPolicy Bypass -File tools/prepare_airplay_windows.ps1
powershell -ExecutionPolicy Bypass -File tools/build_airplay_windows_macast.ps1
pyinstaller --clean --noconfirm Macast.spec
```


## Development under Linux (example: Ubuntu)

### 1. install mpv

```shell
sudo apt install mpv
```

### 2. debug

```shell
pip install -r requirements/common.txt
python Macast.py
# if there is something wrong, try this:
export PYSTRAY_BACKEND=gtk && python3 Macast.py
```

Tips: Make sure you can use **gi**:

```
$ python3
Python 3.7.10 (default, Jun  3 2021, 17:51:26)
Type "help", "copyright", "credits" or "license" for more information.
>>> import gi
>>>
```

if there is something wrong, try: **sudo apt-get install python3-gi**

if you use conda, check this https://stackoverflow.com/a/40303128

For details of GUI support, please refer to: https://pystray.readthedocs.io/en/latest/usage.html#selecting-a-backend


### 3. package

```shell
# build binary
pip install pyinstaller
pyinstaller --noconfirm -F -w --additional-hooks-dir=. --add-data=".version:." --add-data="macast/xml/*:macast/xml"  --add-data="i18n/zh_CN/LC_MESSAGES/*.mo:i18n/zh_CN/LC_MESSAGES" --add-data="assets/*:assets" Macast.py
# build deb
export VERSION=`cat .version`
mkdir -p dist/DEBIAN
mkdir -p dist/usr/bin
mkdir -p dist/usr/share/applications
mkdir -p dist/usr/share/icons
echo -e "Package: Macast\nVersion: ${VERSION}\nArchitecture: amd64\nMaintainer: xfangfang\nDescription: DLNA Media Renderer\nDepends: mpv" > dist/DEBIAN/control
echo -e "[Desktop Entry]\nName=Macast\nComment=DLNA Media Renderer\nExec=/usr/bin/macast\nIcon=/usr/share/icons/Macast.png\nTerminal=false\nType=Application\nCategories=Video" > dist/usr/share/applications/macast.desktop
mv dist/Macast dist/usr/bin/macast
cp assets/icon.png dist/usr/share/icons/Macast.png
dpkg -b dist Macast-v${VERSION}.deb
```


### 4. build with docker (Thanks to **cdrx/docker-pyinstaller**)

Not sure whether it can run normally, used to add support for older versions of Linux.


```shell
cp requirements/common.txt requirements.txt
docker run \
  --env PYPI_INDEX_URL="https://pypi.tuna.tsinghua.edu.cn/simple" \
  --env PYPI_URL="https://pypi.tuna.tsinghua.edu.cn" \
  --rm -v "$(pwd):/src/" xfangfang/pyinstaller-linux:python3 \
    'pip install --upgrade pip &&\
    pip install --no-use-pep517 --upgrade pyinstaller &&\
    pyinstaller --noconfirm -F -w \
      --additional-hooks-dir=. \
      --add-data=".version:." \
      --add-data="macast/xml/*:macast/xml" \
      --add-data="i18n/zh_CN/LC_MESSAGES/*.mo:i18n/zh_CN/LC_MESSAGES" \
      --add-data="assets/*:assets" \
    Macast.py'
```
