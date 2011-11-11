#!/usr/bin/env sh
git init
git add .
GIT_AUTHOR_DATE="2011-11-11T18:00:00" \
GIT_COMMITTER_DATE="2011-11-11T18:00:00" \
git commit -m "111111"
git remote add origin "https://github.com/zzk13180/20111111.git"
git branch -M main
git push -u origin main -f
