PHP_ARG_ENABLE(kislayphp_persistence, whether to enable kislayphp_persistence,
[  --enable-kislayphp_persistence   Enable kislayphp_persistence support])

if test "$PHP_KISLAYPHP_PERSISTENCE" != "no"; then
  PHP_REQUIRE_CXX()
  PHP_ADD_LIBRARY(stdc++,, KISLAYPHP_PERSISTENCE_SHARED_LIBADD)
  PHP_NEW_EXTENSION(kislayphp_persistence, kislayphp_persistence.cpp, $ext_shared)
  PHP_SUBST(KISLAYPHP_PERSISTENCE_SHARED_LIBADD)
fi
