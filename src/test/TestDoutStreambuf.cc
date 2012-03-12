// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2010 Dreamhost
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

/*
 * TestDoutStreambuf
 *
 * Puts some output into the DoutStreambuf class.
 * Check your syslog to see what it did.
 */
#include "common/DoutStreambuf.h"
#include "common/ceph_argparse.h"
#include "common/config.h"
#include "common/debug.h"
#include "global/global_context.h"
#include "global/global_init.h"

#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <syslog.h>

using std::string;

int main(int argc, const char **argv)
{
  vector<const char*> args;
  argv_to_vec(argc, argv, args);
  env_to_vec(args);

  global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);

  DoutStreambuf<char> *dos = new DoutStreambuf<char>();

  {
    std::set <std::string> changed;
    for (const char** t = dos->get_tracked_conf_keys(); *t; ++t) {
      changed.insert(*t);
    }
    DoutLocker _dout_locker;
    dos->handle_conf_change(g_conf, changed);
  }
  derr << "using configuration: " << dos->config_to_str() << dendl;

  std::ostream oss(dos);
  syslog(LOG_USER | LOG_NOTICE, "TestDoutStreambuf: starting test\n");

  dos->set_prio(1);
  oss << "1. I am logging to dout now!" << std::endl;

  dos->set_prio(2);
  oss << "2. And here is another line!" << std::endl;

  oss.flush();

  dos->set_prio(3);
  oss << "3. And here is another line!" << std::endl;

  dos->set_prio(16);
  oss << "4. Stuff ";
  oss << "that ";
  oss << "will ";
  oss << "all ";
  oss << "be ";
  oss << "on ";
  oss << "one ";
  oss << "line.\n";
  oss.flush();

  dos->set_prio(10);
  oss << "5. There will be no blank lines here.\n" << std::endl;
  oss.flush();
  oss.flush();
  oss.flush();

  syslog(LOG_USER | LOG_NOTICE, "TestDoutStreambuf: ending test\n");

  return 0;
}
