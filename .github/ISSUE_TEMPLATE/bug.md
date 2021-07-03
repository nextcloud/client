---
name: Bugs
about: Crashes and other bugs
labels: 'bug'

---

<!--
Thanks for reporting issues back to Nextcloud!

This is the **issue tracker of Nextcloud**, please do NOT use this to get answers to your questions or get help for fixing your installation. You can find help debugging your system on our home user forums: https://help.nextcloud.com or, if you use Nextcloud in a large organization, ask our engineers on https://portal.nextcloud.com. See also  https://nextcloud.com/support for support options.

Guidelines for submitting issues:

* Please search the existing issues first, it's likely that your issue was already reported or even fixed.
    - Go to https://github.com/nextcloud and type any word in the top search/command bar. You probably see something like "We couldn’t find any repositories matching ..." then click "Issues" in the left navigation.
    - You can also filter by appending e. g. "state:open" to the search string.
    - More info on search syntax within github: https://help.github.com/articles/searching-issues
    
* Please fill in as much of the template below as possible. The logs are absolutely crucial for the developers to be able to help you. Expect us to quickly close issues without logs or other information we need. 

* Also note that we have a https://nextcloud.com/contribute/code-of-conduct/ that applies on Github. To summarize it: be kind. We try our best to be nice, too. If you can't be bothered to be polite, please just don't bother to report issues as we won't feel motivated to help you. 
-->

<!--- Please keep the note below for others who read your bug report -->

## How to use GitHub

* Please use the 👍 [reaction](https://blog.github.com/2016-03-10-add-reactions-to-pull-requests-issues-and-comments/) to show that you are affected by the same issue.
* Please don't comment if you have no relevant information to add. It's just extra noise for everyone subscribed to this issue.
* Subscribe to receive notifications on status change and new comments. 

## Expected behaviour
Tell us what should happen

## Actual behaviour
Tell us what happens instead

## Steps to reproduce
1.
2.
3.

## Client configuration
Client version:
<!---
Please try to only report a bug if it happens with the latest version
The latest version can be seen by checking https://nextcloud.com/install/#install-clients
--->

Operating system:

OS language:

Qt version used by client package (Linux only, see also Settings dialog):

Client package (From Nextcloud or distro) (Linux only):

Installation path of client:

## Server configuration
<!---
Optional section. It depends on the issue.
--->
Nextcloud version:

Storage backend (external storage):

## Logs
<!-- desktop client logs are a hard requirement for bug reports because we don't know how to do magic here :) -->

Please use Gist (https://gist.github.com/) or a similar code paster for longer
logs.

1. Client logfile: 
Since 3.1: Under the "General" settings, you can click on "Create Debug Archive ..." to pick the location of where the desktop client will export the logs and the database to a zip file.
On previous releases: Via the command line: `nextcloud --logdebug --logwindow` or `nextcloud --logdebug --logfile log.txt`
(See also https://docs.nextcloud.com/desktop/3.0/troubleshooting.html#log-files)

2. Web server error log:

3. Server logfile: nextcloud log (data/nextcloud.log):
