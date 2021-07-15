Appendix B: History and Architecture
====================================

.. index:: architecture

Nextcloud provides desktop sync clients to synchronize the contents of local
directories from computers, tablets, and handheld devices to the Nextcloud
server.

Synchronization is accomplished using csync_, a bidirectional file
synchronizing tool that provides both a command line client as well as a
library. A special module for csync was written to synchronize with the
Nextcloud built-in WebDAV server.

The Nextcloud Client software is written in C++ using the `Qt Framework`_. As a
result, the Nextcloud Client runs on Linux, Windows, and MacOS.

.. _csync: http://www.csync.org
.. _`Qt Framework`: http://www.qt-project.org

The Synchronization Process
---------------------------

The process of synchronization keeps files in two separate repositories the 
same. When synchronized:

- If a file is added to one repository it is copied to the other synchronized repository.
- When a file is changed in one repository, the change is propagated to any other
  synchronized repository.
- If a file is deleted in one repository, it is deleted in any other.

It is important to note that the Nextcloud synchronization process does not use
a typical client/server system where the server is always master.  This is a
major difference between the Nextcloud synchronization process and other systems
like a file backup, where only changes to files or folders and the addition of
new files are propagated, but these files and folders are never deleted unless
explicitly deleted in the backup.

During synchronization, the Nextcloud Client checks both repositories for
changes frequently. This process is referred to as a *sync run*. In between
sync runs, the local repository is monitored by a file system monitoring
process that starts a sync run immediately if something was edited, added, or
removed.

Synchronization by Time versus ETag
-----------------------------------
.. index:: time stamps, file times, etag, unique id

Until the release of the client version 1.1, the Nextcloud
synchronization process employed a single file property -- the file modification
time -- to decide which file was newer and needed to be synchronized to the
other repository.

The *modification timestamp* is part of the files metadata. It is available on
every relevant filesystem and is the typical indicator for a file change.
Modification timestamps do not require special action to create, and have a
general meaning. One design goal of csync is to not require a special server
component. This design goal is why csync was chosen as the backend component.

To compare the modification times of two files from different systems, csync
must operate on the same base. Before client version 1.1.0, csync
required both device repositories to run on the exact same time.  This
requirement was achieved through the use of enterprise standard `NTP time
synchronization`_ on all machines.

Because this timing strategy is rather fragile without the use of NTP, the Nextcloud
server provides a unique number that changes whenever the file
changes. Although this number is a unique value, it is not a hash of the file.
Instead, it is a randomly chosen number, that is transmitted in the Etag_
field. Because the file number changes if the file changes, its use is
guaranteed to determine if one of the files has changed and, thereby, launching
a synchronization process.

Before the 1.3.0 release of the Desktop Client, the synchronization process
might create false conflict files if time deviates. Original and changed files
conflict only in their timestamp, but not in their content. This behavior was
changed to employ a binary check if files differ.

Like files, directories also hold a unique ID that changes whenever one of the
contained files or directories is modified. Because this is a recursive
process, it significantly reduces the effort required for a synchronization
cycle, because the client only analyzes directories with a modified ID.

.. _`NTP time synchronization`: http://en.wikipedia.org/wiki/Network_Time_Protocol
.. _Etag: http://en.wikipedia.org/wiki/HTTP_ETag

Comparison and Conflict Cases
-----------------------------

As mentioned above, during a *sync run* the client must first detect if one of
the two repositories have changed files. On the local repository, the client
traverses the file tree and compares the modification time of each file with an
expected value stored in its database. If the value is not the same, the client
determines that the file has been modified in the local repository.

.. note:: On the local side, the modification time is a good attribute to use for 
   detecting changes, because
   the value does not depend on time shifts and such.

For the remote (that is, Nextcloud server) repository, the client compares the
ETag of each file with its expected value. Again, the expected ETag value is
queried from the client database. If the ETag is the same, the file has not
changed and no synchronization occurs.

In the event a file has changed on both the local and the remote repository
since the last sync run, it can not easily be decided which version of the file
is the one that should be used. However, changes to any side will not be lost.  Instead,
a *conflict case* is created. The client resolves this conflict by renaming the
local file, appending a conflict label and timestamp, and saving the remote file
under the original file name.

Example: Assume there is a conflict in message.txt because its contents have
changed both locally and remotely since the last sync run. The local file with
the local changes will be renamed to message_conflict-20160101-153110.txt and
the remote file will be downloaded and saved as message.txt.

Conflict files are always created on the client and never on the server.

..
  Checksum Algorithm Negotiation
  ------------------------------

  In ownCloud 10.0 we implemented a checksum feature which checks the file integrity on upload and download by computing a checksum after the file transfer finishes.
  The client queries the server capabilities after login to decide which checksum algorithm to use.
  Currently, SHA1 is hard-coded in the official server release and can't be changed by the end-user. 
  Note that the server additionally also supports MD5 and Adler-32, but the desktop client will always use the checksum algorithm announced in the capabilities:

  ::

    GET http://localhost:8000/ocs/v1.php/cloud/capabilities?format=json

  ::

    json
    {
      "ocs":{
          "meta":{
            "status":"ok",
            "statuscode":100,
            "message":"OK",
            "totalitems":"",
            "itemsperpage":""
          },
          "data":{
            "version":{
                "major":10,
                "minor":0,
                "micro":0,
                "string":"10.0.0 beta",
                "edition":"Community"
            },
            "capabilities":{
                "core":{
                  "pollinterval":60,
                  "webdav-root":"remote.php/dav"
                },
                "dav":{
                  "chunking":"1.0"
                },
                "files_sharing":{
                  "api_enabled":true,
                  "public":{
                      "enabled":true,
                      "password":{
                        "enforced":false
                      },
                      "expire_date":{
                        "enabled":false
                      },
                      "send_mail":false,
                      "upload":true
                  },
                  "user":{
                      "send_mail":false
                  },
                  "resharing":true,
                  "group_sharing":true,
                  "federation":{
                      "outgoing":true,
                      "incoming":true
                  }
                },
                "checksums":{
                  "supportedTypes":[
                      "SHA1"
                  ],
                  "preferredUploadType":"SHA1"
                },
                "files":{
                  "bigfilechunking":true,
                  "blacklisted_files":[
                      ".htaccess"
                  ],
                  "undelete":true,
                  "versioning":true
                }
            }
          }
      }
    }

  Upload
  ~~~~~~

  A checksum is calculated with the previously negotiated algorithm by the client and sent along with the file in an HTTP Header. 
  ```OC-Checksum: [algorithm]:[checksum]```

  .. image:: ./images/checksums/client-activity.png

  During file upload, the server computes SHA1, MD5, and Adler-32 checksums and compares one of them to the checksum supplied by the client. 

  On mismatch, the server returns HTTP Status code 400 (Bad Request) thus signaling the client that the upload failed. 
  The server then discards the upload, and the client blacklists the file:

  .. image:: ./images/checksums/testing-checksums.png

  ::

    <?xml version='1.0' encoding='utf-8'?>
    <d:error xmlns:d="DAV:" xmlns:s="http://sabredav.org/ns">
      <s:exception>Sabre\DAV\Exception\BadRequest</s:exception>
      <s:message>The computed checksum does not match the one received from the
    client.</s:message>
    </d:error>

  The client retries the upload using exponential back-off. 
  On success (matching checksum) the computed checksums are stored by the server in ``oc_filecache`` alongside the file.

  Chunked Upload
  ~~~~~~~~~~~~~~

  Mostly same as above. 
  The checksum of the full file is sent with every chunk of the file. 
  But the server only compares the checksum after receiving the checksum sent with the last chunk.

  Download
  ~~~~~~~~

  The server sends the checksum in an HTTP header with the file. (same format as above).
  If no checksum is found in ``oc_filecache`` (freshly mounted external storage) it is computed and stored in ``oc_filecache`` on the first download. 
  The checksum is then provided on all subsequent downloads but not on the first. 

.. _ignored-files-label:

Ignored Files
-------------

The Nextcloud Client supports the ability to exclude or ignore certain files from the synchronization process. 
Some system wide file patterns that are used to exclude or ignore files are included with the client by default and the Nextcloud Client provides the ability to add custom patterns.

By default, the Nextcloud Client ignores the following files:

* Files matched by one of the patterns defined in the Ignored Files Editor.
* Files starting with ``._sync_*.db*``, ``.sync_*.db*``, ``.csync_journal.db*``, ``.owncloudsync.log*``,  as these files are reserved for journalling.
* Files with a name longer than 254 characters.
* The file ``Desktop.ini`` in the root of a synced folder.
* Files matching the pattern ``*_conflict-*`` unless conflict file uploading is enabled.
* Windows only: Files containing characters that do not work on typical Windows filesystems ``(`\, /, :, ?, *, ", >, <, |`)``.
* Windows only: Files with a trailing space or dot.
* Windows only: Filenames that are reserved on Windows.

If a pattern selected using a checkbox in the `ignoredFilesEditor-label` (or if
a line in the exclude file starts with the character ``]`` directly followed by
the file pattern), files matching the pattern are considered *fleeting meta
data*. 

These files are ignored and *removed* by the client if found in the
synchronized folder. 
This is suitable for meta files created by some applications that have no sustainable meaning.

If a pattern ends with the forward slash (``/``) character, only directories are matched. 
The pattern is only applied for directory components of filenames selected using the checkbox.

To match filenames against the exclude patterns, the UNIX standard C library
function ``fnmatch`` is used. 
This process checks the filename against the specified pattern using standard shell wildcard pattern matching. 
For more information, please refer to `The opengroup website
<http://pubs.opengroup.org/onlinepubs/009695399/utilities/xcu_chap02.html#tag_02_13_01>`_.

The path that is checked is the relative path under the sync root directory.

**Pattern and File Match Examples:**

+-----------+------------------------------+
| Pattern   | File Matches                 |
+===========+==============================+
| ``~$*``   | ``~$foo``, ``~$example.doc`` |
+-----------+------------------------------+
| ``fl?p``  | ``flip``, ``flap``           |
+-----------+------------------------------+
| ``moo/``  | ``map/moo/``, ``moo/``       |
+-----------+------------------------------+


The Sync Journal
----------------

The client stores the ETag number in a per-directory database, called the
*journal*.  This database is a hidden file contained in the directory to be
synchronized.

If the journal database is removed, the Nextcloud Client CSync backend rebuilds
the database by comparing the files and their modification times. This process
ensures that both server and client are synchronized using the appropriate NTP
time before restarting the client following a database removal.

Custom WebDAV Properties
------------------------

In the communication between client and server a couple of custom WebDAV properties
were introduced. They are either needed for sync functionality or help have a positive
effect on synchronization performance.

This chapter describes additional XML elements which the server returns in response
to a successful PROPFIND request on a file or directory. The elements are returned in
the namespace ``oc``.

Server Side  Permissions
------------------------

The XML element ``<oc:permissions>`` represents the permission- and sharing state of the
item. It is a list of characters, and each of the chars has a meaning as outlined
in the table below:

+------+----------------+-------------------------------------------+
| Code |   Resource     |  Description                              |
+------+----------------+-------------------------------------------+
| S    | File or Folder | is shared                                 |
+------+----------------+-------------------------------------------+
| R    | File or Folder | can share (includes re-share)             |
+------+----------------+-------------------------------------------+
| M    | File or Folder | is mounted (like on Dropbox, Samba, etc.) |
+------+----------------+-------------------------------------------+
| W    | File           | can write file                            |
+------+----------------+-------------------------------------------+
| C    | Folder         | can create file in folder                 |
+------+----------------+-------------------------------------------+
| K    | Folder         | can create folder (mkdir)                 |
+------+----------------+-------------------------------------------+
| D    | File or Folder | can delete file or folder                 |
+------+----------------+-------------------------------------------+
| N    | File or Folder | can rename file or folder                 |
+------+----------------+-------------------------------------------+
| V    | File or Folder | can move file or folder                   |
+------+----------------+-------------------------------------------+


Example:

  <oc:permissions>RDNVCK</oc:permissions>

File- or Directory Size
-----------------------

The XML element ``<oc:size>`` represents the file- or directory size in bytes. For
directories, the size of the whole file tree underneath the directory is accumulated.

Example:

  <oc:size>2429176697</oc:size>

FileID
------

The XML element ``<oc:id>`` represents the so called file ID. It is a non volatile string id
that stays constant as long as the file exists. It is not changed if the file changes or
is renamed or moved.

Example:

  <oc:id>00000020oc5cfy6qqizm</oc:id>
  
End-to-end Encryption
---------------------

Nextcloud is built around the fundamental assumption that, as you can host your own Nextcloud server, you can trust it with your data. This assumption means data on the Nextcloud server can be provided to users through a browser interface. Users can browse their files online, access their calendars and mail and other data from the respective apps and share and collaboratively edit documents with others including guests and users without an account. While data on the server can be encrypted, this is largely designed to protect it from malicious storage solutions or theft of the whole hardware. System administrators always have access to the data.

But for a subset of data, this assumption of trust might not hold true. For example, at an enterprise, the documents of the Human Resources department or the financial department are too sensitive to allow system administrators who manage the server, access them. As a private user, you might trust your hosting provider with the vast majority of your data but not with medical records. And even if there is trust in the server administration team, a breach of the server can never entirely be ruled out and for some data, even a tiny risk is unacceptable.

The Nextcloud End-to-end Encryption feature (E2EE) was designed to offer protection against a full compromise of the server. See for more details our blog about the `threat model for the encryption solutions in Nextcloud`_ and our `webpage about End-to-end Encryption`_. If the end-to-end encryption app is enabled on the server, users can use one of the clients to select a local folder and enable this feature. This will ensure the client encrypts data before it is transmitted to the server.

The first time E2EE is enabled on a folder in any of the clients, the user is prompted with a private key consisting of 12 security words. The user is strongly recommended to record these somewhere secure as the complete loss of this private key means there is no way to access their data anymore. The key is also securely stored in the device's key storage and can be shown on demand. Making the folder available on a second device requires entering this key. Future versions of Nextcloud clients will be able to display a QR code to simplify the process of adding devices. Sharing with other users will not require any special keys or passwords.

Encrypting files locally means the server has no access to them. This brings with it a number of limitations:

* E2EE files can not be accessed or previewed through the web interface
* E2EE files can not be edited with Online Office solutions
* E2EE files can not be shared with a public link
* E2EE files can not be searched, tagged, commented on and have no versioning or trash bin
* E2EE files can not be accessed in other Nextcloud Apps. This means they have no chat sidebar, can not be attached to emails or deck cards, shared in Talk rooms and so on
* E2EE results in slower syncing of file and works poorly or not at all with large files and large quantities of files

These limitations are fundamental to how securely implemented end-to-end encryption works. We realize there are some solutions that call their technology 'end-to-end encryption' but with browser access. Reality is that offering browser access to end-to-end encrypted files would essentially negate any of the benefits of end-to-end encryption. Serving a file in the browser means the server needs to be able to read the files. But if the server can read the files, administrator or a malicious attacker who gained access to the server, can too. Decrypting the file in the browser does not solve this security risk in the least, as the javascript code that would be needed to decrypt the file comes FROM the server, and of course a compromised server would simply send modified javascript code which sends a copy of the encryption keys to the attacker without anybody noticing. See for more details our blog about the `threat model for the encryption solutions in Nextcloud`_ and our `webpage about End-to-end Encryption`_.

The E2EE design of Nextcloud allows for sharing on a per-folder level to individual users (not groups), but, as of early 2021, this feature is still on the road map for implementation in the clients.

Due to all these limitations that are inherent to true end-to-end encryption, it is only recommended for a small subset of files, in just a small number of folders. Encrypting your entire sync folder is likely to result in poor performance and sync errors and if you do not trust your server at all, Nextcloud is perhaps not the right solution for your use case. You might instead want to use encrypted archives or another solution.

.. note::
    * End-to-end Encryption works with Virtual Files (VFS) but only on a per-folder level. Folders with E2EE have to be made available offline in their entirety to access the files, they can not be retrieved on demand in the folder.

.. _`webpage about End-to-end Encryption`: http://nextcloud.com/endtoend
.. _`threat model for the encryption solutions in Nextcloud`: https://nextcloud.com/blog/encryption-in-nextcloud/

Virtual Files
-------------

.. note::
    * This feature is currently only available on ``Windows`` by default. ``Linux`` implementation is experimental and must be enabled by adding ``showExperimentalOptions=true`` to the ``nextcloud.cfg`` configuration file in the ``App Data`` folder. ``macOS``, at the moment, is using the same backend as ``Linux`` one. It can be enabled with the same ``showExperimentalOptions`` flag.

Oftentimes, users are working with a huge amount of files that are big in size. Synchronizing every such file to a device that's running a Nextcloud desktop client is not always possible due to the user's device storage space limitation.
Let's assume that your desktop client is connected to a server that has 1TB of data. You want all those files at hand, so you can quickly access any file via the file explorer. Your device has 512GB local storage device.
Obviously, it's not possible to synchronize even half of 1TB of data that is on the server. What should you do in this case? Of course, you can just utilize the Selective Sync feature, and keep switching between different folders, in such a way that you only synchronize those folders that you are currently working with.
Needless to say, this is far from being convenient.

That's why, starting from 3.2.0, we are introducing the VFS (Virtual Files) feature. You may have had experience working with a similar feature in other cloud sync clients. This feature is known by different names: Files On-Demand, SmartSync, etc.
The VFS does not occupy much space on the user's storage. It just creates placeholders for each file and folder. These files are quite small and only contain metadata needed to display them properly and to fetch the actual file when needed.

One will see a hydration (in other words - file download) process when double-clicking on a file that must become available. There will be a progress-bar popup displayed if the file is large enough. So, the hydration process can be observed and it makes it easy to then find out, how long, it would take to fetch the actual file from the server.
The "Hydration" can be thought of as "downloading" or "fetching" the file contents. As soon as hydration is complete, the file will then be opened normally as now it is a real file on the user's storage. It won't disappear, and, from now on, will always be available, unless it is manually dehydrated.

.. image:: images/vfs_hydration_progress_bar.png
   :alt: VFS hydration progress bar

As long as the VFS is enabled, a user can choose to remove files that are no longer needed from the local storage. This can be achieved by right-clicking the file/folder in the explorer, and then, choosing "Free up local space" from the context menu.
Alternatively, space can be freed up by right-clicking the sync folder in the Settings dialog. It is also possible to make files always hydrated, or, in other words, always available locally. A user just needs to choose the "Make always available locally" option in the aforementioned context menus.

.. image:: images/vfs_context_menu_options.png
   :alt: VFS context menu options

The VFS can also be disabled if needed, so, the entire folder will then be synced normally. This option is available in the context menu of a sync folder in the Settings dialog. Once disabled, the VFS can also be enabled back by using the same context menu.
Files that must be removed from the local storage only, need to be dehydrated via the "Free up local space" option, so, the placeholder will get created in place of real files.

.. note::
    * End-to-end Encryption works with Virtual Files (VFS) but only on a per-folder level. Folders with E2EE can be made available offline in their entirety, but the individual files in them can not be retrieved on demand. This is mainly due to two technical reasons. First, the Windows VFS API is not designed for handling encrypted files. Second, while the VFS is designed to deal mostly with large files, E2EE is mostly recommended for use with small files as encrypting and decrypting large files puts large demands on the computer infrastructure.


User Status
-----------

Starting from 3.2.0, user status is displayed in the Nextcloud desktop client's tray window. The icon and a text message are displayed as long as those are set in the user's account menu in the Web UI (server's website). At the moment, setting the status from the desktop client is not available.
The status is updated almost immediately after it is set in the Web UI. Default user status is always "Online" if no other status is available from the server-side.

.. image:: images/status_feature_example.png
   :alt: User Status feature in the tray window
