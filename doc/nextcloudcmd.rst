The Nextcloud Client packages contain a command line client, ``nextcloudcmd``, that can 
be used to synchronize Nextcloud files to client machines.

``nextcloudcmd`` performs a single *sync run* and then exits the synchronization 
process. In this manner, ``nextcloudcmd`` processes the differences between 
client and server directories and propagates the files to bring both 
repositories to the same state. Contrary to the GUI-based client, 
``nextcloudcmd`` does not repeat synchronizations on its own. It also does not 
monitor for file system changes.


Install ``nextcloudcmd``
~~~~~~~~~~~~~~~~~~~~~~~~

CentOS

::

    $ sudo yum -y install epel-release
    $ sudo yum -y install nextcloud-client

Ubuntu/Debian

::

    $ sudo add-apt-repository ppa:nextcloud-devs/client
    $ sudo apt update
    $ sudo apt install nextcloud-client


Refer to the link

- https://nextcloud.com/install/#install-clients
- https://launchpad.net/~nextcloud-devs/+archive/ubuntu/client
- https://pkgs.alpinelinux.org/packages?name=nextcloud-client
- https://help.nextcloud.com/t/linux-packages-status/10216


To invoke ``nextcloudcmd``, you must provide the local and the remote repository 
URL using the following command::

  nextcloudcmd [OPTIONS...] sourcedir nextcloudurl

where ``sourcedir`` is the local directory and ``nextcloudurl`` is
the server URL.

Other command line switches supported by ``nextcloudcmd`` include the following:

``--user``, ``-u`` ``[user]``
       Use ``user`` as the login name.

``--password``, ``-p`` ``[password]``
       Use ``password`` as the password.

``-n``
       Use ``netrc (5)`` for login.

``--non-interactive``
       Do not prompt for questions.

``--silent``, ``--s``
       Inhibits verbose log output.

``--trust``
       Trust any SSL certificate, including invalid ones.

``--httpproxy  http://[user@pass:]<server>:<port>``
      Uses ``server`` as HTTP proxy.

``--nonshib``
      Uses Non Shibboleth WebDAV Authentication

``--davpath [path]``
      Overrides the WebDAV Path with ``path``

``--exclude [file]``
      Exclude list file

``--unsyncedfolders [file]``
      File containing the list of un-synced remote folders (selective sync)

``--max-sync-retries [n]``
      Retries maximum n times (defaults to 3)

``-h``
      Sync hidden files, do not ignore them

Credential Handling
~~~~~~~~~~~~~~~~~~~

``nextcloudcmd`` requires the user to specify the username and password using the standard URL pattern, e.g., 

::

  $ nextcloudcmd /home/user/my_sync_folder https://carla:secret@server/nextcloud/remote.php/dav/

To synchronize the Nextcloud directory ``Music`` to the local directory
``media/music``, through a proxy listening on port ``8080``, and on a gateway
machine using IP address ``192.168.178.1``, the command line would be::

  $ nextcloudcmd --httpproxy http://192.168.178.1:8080 \
                $HOME/media/music \
                https://server/nextcloud/remote.php/dav/Music

``nextcloudcmd`` will prompt for the user name and password, unless they have
been specified on the command line or ``-n`` has been passed.

Exclude List
~~~~~~~~~~~~

``nextcloudcmd`` requires access to an exclude list file. It must either be
installed along with ``nextcloudcmd`` and thus be available in a system location,
be placed next to the binary as ``sync-exclude.lst`` or be explicitly specified
with the ``--exclude`` switch.

Example
~~~~~~~~~~~~

- Synchronize a local directory to the specified directory of the nextcloud server

::

    $ nextcloudcmd /home/user/<my_sync_folder> \
    https://<username>:<secret>@<server_address>/remote.php/dav/<Directory_that_has_been_created>
