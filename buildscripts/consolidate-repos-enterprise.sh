#!/bin/bash
#
# consolidate-repos-enterprise.sh
#
# Create new repo directory under /var/www-enterprise/repo.consolidated
# containing every deb and every rpm under /var/www-enterprise/ with proper
# repo metadata for apt and yum 
#

source_dir=/var/www-enterprise/

repodir=/var/www-enterprise/repo.consolidated

gpg_recip='<richard@10gen.com>' 

echo "Using directory: $repodir"

# set up repo dirs if they don't exist
#
for distro in debian-sysvinit redhat ubuntu-upstart
do
  mkdir -p "$repodir/$distro"
done

tempfile=`mktemp /tmp/consolidate-repos-enterprise.XXXXXX`

mkdir -p "$repodir"

find "$source_dir" -name \*.rpm -o -name \*.deb -o -name Release | grep -v "$repodir" > "$tempfile"

echo "Scanning and copying package files from $source_dir"
echo ". = skipping existing file, @ = copying file"
while read package
do
  new_package_location="$repodir`echo \"$package\" | sed 's/\/var\/www-enterprise\/[^\/]*//;'`"

  # skip if the directory structure looks weird
  #
  if [ "`echo \"$new_package_location\" | grep /repo/`" ]
  then
    continue
  fi  

  # skip if not enterprise package
  #
  if [ ! "`echo \"$new_package_location\" | grep enterprise`" ]
  then
    continue
  fi 
  # skip if it's already there 
  #
  if [ -e "$new_package_location" -a "`basename \"$package\"`" != "Release" ]
  then
      echo -n .
  else
    mkdir -p "`dirname \"$new_package_location\"`"
    echo -n @
    cp "$package" "$new_package_location"
  fi
done < "$tempfile"
echo

# packages are in place, now create metadata
#
for debian_dir in "$repodir"/ubuntu-* "$repodir"/debian-* 
do
  cd "$debian_dir" 
  for arch_dir in dists/dist/10gen/{binary-i386,binary-amd64}
  do
    echo "Generating Packages file under $debian_dir/$arch_dir"
    if [ ! -d $arch_dir ]
    then
      mkdir $arch_dir
    fi
    dpkg-scanpackages --multiversion "$arch_dir"   > "$arch_dir"/Packages
    gzip -9c  "$arch_dir"/Packages >  "$arch_dir"/Packages.gz
  done

  for release_dir in "$debian_dir"/dists/dist
  do
    echo "Generating Release file under $release_dir"
    cd $release_dir
    tempfile=`mktemp /tmp/ReleaseXXXXXX`
    tempfile2=`mktemp /tmp/ReleaseXXXXXX`
    mv Release $tempfile
    head -9 $tempfile > $tempfile2
    apt-ftparchive release . >> $tempfile2
    cp $tempfile2 Release
    chmod 644 Release
    rm Release.gpg
    echo "Signing Release file"
    gpg -r "$gpg_recip" --no-secmem-warning -abs --output Release.gpg  Release
  done
done

for redhat_dir in $(find "$repodir"/redhat -name x86_64 -o -name i386)
do
  echo "Generating redhat repo metadata under $redhat_dir"
  cd "$redhat_dir"
  createrepo .
done


