echo "#########################################################"
echo "# jkcoxson and spidy123222's DolphiniOS install script. Edited by Matako for PiOS #"
echo "#########################################################\n\n\n"

if [ "$EUID" -ne 0 ]
  then echo "Please run as root. Exiting..."
  exit
fi

echo "Root check passed, continue install."


apt-get install checkinstall -y

apt-get install autoconf -y

apt install libtool -y

apt-get install libtool-bin -y

apt-get install libusb-1.0-0-dev -y

apt-get install libssl-dev -y

apt-get install libavahi-client-dev -y

apt-get install python-plist -y

apt-get install doxygen -y

apt-get install cython -y

apt-get update -y

apt-get install libplist++ -y


git clone https://github.com/hjsm23/libplist

git clone https://github.com/hjsm23/libusbmuxd

git clone https://github.com/hjsm23/libimobiledevice

git clone https://github.com/hjsm23/libgeneral.git

git clone https://github.com/hjsm23/usbmuxd2.git

cd libplist
./autogen.sh
make && make install
ldconfig
cd ..

cd libusbmuxd
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig ./autogen.sh
make && make install
sudo ldconfig
cd ..

cd libimobiledevice
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig ./autogen.sh --enable-debug
make && make install
cd ..

cd libgeneral
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig ./autogen.sh --enable-debug
make && make install
cd ../

cd usbmuxd2
git submodule init
git submodule update
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig ./autogen.sh --prefix=/usr --sysconfdir=/etc --localstatedir=/var --runstatedir=/run
make && make install

killall usbmuxd


echo
echo
echo
echo "##############################"
echo "#   Installation Completed.  #"
echo "##############################\n\n\n"

usbmuxd -s -d --user root start

idevice_id
