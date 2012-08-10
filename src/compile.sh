make bin/undionly.kpxe DEBUG=malloc:0,tcp:1,interface:0,open:1,ipv4:0,netdevice:0,xfer:0,resolv:0,refcnt:0,downloader:1
cp bin/undionly.kpxe /var/lib/tftpboot/undionly.kpxe
