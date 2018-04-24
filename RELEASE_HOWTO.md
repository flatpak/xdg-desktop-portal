# Steps for doing a xdg-desktop-portal release

 - git clean -fxd
 - ./autogen.sh && make -C po/ update-po
 - git add po/*po &&  git commit -m "Update po files"

 - git clean -fxd
 - bump version in configure.ac
 - add content to NEWS
 - git commit -m <version>
 - git push origin master
 - ./autogen.sh --enable-docbook-docs 
 - make all
 - make distcheck
 - git tag <version>
 - git push origin refs/tags/<version>
 - upload tarball to github as release
 - edit release, copy NEWS section in
