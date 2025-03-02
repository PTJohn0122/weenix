# Weenix

[![](https://skillicons.dev/icons?i=c,clion,cmake,redhat,debian)](https://skillicons.dev)

Weenix is a virtual machine that can run on Redhat-derived or Debian-derived Linux flavors. 

This repository is a backup from a private reposity that belongs to a private organization.

## Installation

If you're using a virtual machine with the weenix Vagrantfile, the dependencies should be installed automatically when the machine is provisioned.

### Download and install dependencies.

   On recent versions of Ubuntu or Debian, you can simply run:

   ```bash
   $ sudo apt-get install git-core build-essential gcc gdb qemu genisoimage make python python-argparse cscope xterm bash grub xorriso
   ```

   or on Redhat:

   ```bash
   $ sudo yum install git-core gcc gdb qemu genisoimage make python python-argparse cscope xterm bash grub2-tools xorriso
   ```

### Compile Weenix:

   ```bash
   $ make
   ```

### Invoke Weenix:

   ```bash
   $ ./weenix -n
   ```

   or, to run Weenix under gdb, run:

   ```bash
   $ ./weenix -n -d gdb
   ```
   You may also need to install `pyelftools`, to do so, make sure that you have pip3 installed. Once you have that installed, you can run `pip3 install pyelftools`. 
