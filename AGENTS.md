# Rig workflow

Applies to every repo in the JPEG XS camera rig.

1. The main repo checkout ALWAYS remains on the default branch. Never change the
   branch checked out there: no `git checkout`, `git switch`, `git stash` or
   `git clean`. Another agent may be working in that directory.
2. All work happens in a branch worktree:

       git worktree add ../<repo>-<branch> -b <branch>

   Edit, build and commit only in your own worktree.
3. Land work by MERGING YOUR WORKTREE BRANCH INTO THE DEFAULT BRANCH, from the main
   checkout, once the work is complete. That is the landing procedure - it is not a
   breach of rule 1.
4. Never commit directly onto the default branch.