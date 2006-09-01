
#include "include/types.h"

#include "Ager.h"
#include "ObjectStore.h"

#include "config.h"
#include "common/Clock.h"

// ick
#include "ebofs/Ebofs.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


int myrand() 
{
  if (0) 
	return rand();
  else {
	static int n = 0;
	srand(n++);
	return rand();
  }
}


object_t Ager::age_get_oid() {
  if (!age_free_oids.empty()) {
	object_t o = age_free_oids.front();
	age_free_oids.pop_front();
	return o;
  }
  return age_cur_oid++;
}

ssize_t Ager::age_pick_size() {
  ssize_t max = file_size_distn.sample() * 1024;
  return max/2 + (myrand() % 100) * max/200 + 1;
}

bool start_debug = false;

__uint64_t Ager::age_fill(float pc, utime_t until) {
  int max = 1024*1024;
  char *buf = new char[max];
  bufferlist bl;
  bl.push_back(new buffer(buf, max));
  __uint64_t wrote = 0;
  while (1) {
	if (g_clock.now() > until) break;
	
	struct statfs st;
	store->statfs(&st);
	float a = (float)(st.f_blocks-st.f_bavail) / (float)st.f_blocks;
	//dout(10) << "age_fill at " << a << " / " << pc << " .. " << st.f_blocks << " " << st.f_bavail << endl;
	if (a >= pc) {
	  dout(2) << "age_fill at " << a << " / " << pc << " stopping" << endl;
	  break;
	}
	
	object_t oid = age_get_oid();
	
	int b = myrand() % 10;
	age_objects[b].push_back(oid);
	
	ssize_t s = age_pick_size();
	wrote += (s + 4095) / 4096;
	
	dout(2) << "age_fill at " << a << " / " << pc << " creating " << hex << oid << dec << " sz " << s << endl;
	

	if (false && !g_conf.ebofs_verify && start_debug && wrote > 1000000ULL) { 
	  /*


	  1005700
?
1005000
1005700
      1005710
      1005725ULL
      1005750ULL
      1005800
	  1006000

//  99  1000500 ? 1000750 1006000
*/
	  g_conf.debug_ebofs = 30;
	  g_conf.ebofs_verify = true;	  
	}

	off_t off = 0;
	while (s) {
	  ssize_t t = MIN(s, max);
	  bufferlist sbl;
	  sbl.substr_of(bl, 0, t);
	  store->write(oid, off, t, sbl, false);
	  off += t;
	  s -= t;
	}
	oid++;
  }
  delete[] buf;

  return wrote;
}

void Ager::age_empty(float pc) {
  int nper = 20;
  int n = nper;

  //g_conf.ebofs_verify = true;

  while (1) {
	struct statfs st;
	store->statfs(&st);
	float a = (float)(st.f_blocks-st.f_bfree) / (float)st.f_blocks;
	dout(2) << "age_empty at " << a << " / " << pc << endl;//" stopping" << endl;
	if (a <= pc) {
	  dout(2) << "age_empty at " << a << " / " << pc << " stopping" << endl;
	  break;
	}
	
	int b = myrand() % 10;
	n--;
	if (n == 0 || age_objects[b].empty()) {
	  dout(2) << "age_empty sync" << endl;
	  //sync();
	  sync();
	  n = nper;
	  continue;
	}
	object_t oid = age_objects[b].front();
	age_objects[b].pop_front();
	
	dout(2) << "age_empty at " << a << " / " << pc << " removing " << hex << oid << dec << endl;
	
	store->remove(oid);
	age_free_oids.push_back(oid);
  }

  g_conf.ebofs_verify = false;
}

void pfrag(ObjectStore::FragmentationStat &st)
{
  cout << st.num_extent << "\tavg\t" << st.avg_extent
	   << "\t" << st.avg_extent_per_object << "\tper obj,\t" 
	   << st.avg_extent_jump << "\t jump,\t"
	   << st.num_free_extent << "\tfree avg\t" << st.avg_free_extent;
  
  /*
	for (map<int,int>::iterator p = st.free_extent_dist.begin();
	p != st.free_extent_dist.end();
	p++) 
	cout << "\t" << p->first << "=\t" << p->second;
	cout << endl;
  */
  
  int n = st.num_free_extent;
  for (__uint64_t i=2; i <= 30; i += 2) {
	cout << "\t" 
	  //<< i
	  //<< "=\t" 
		 << st.free_extent_dist[i];
	n -= st.free_extent_dist[i];
	if (n == 0) break;
  }
  cout << endl;
}


void Ager::age(int time,
			   float high_water,    // fill to this %
			   float low_water,     // then empty to this %
			   int count,         // this many times
			   float final_water,   // and end here ( <= low_water)
			   int fake_size_mb) { 

  store->_fake_writes(true);
  srand(0);

  utime_t start = g_clock.now();
  utime_t until = start;
  until.sec_ref() += time;
  
  int elapsed = 0;
  int freelist_inc = 60;
  utime_t nextfl = start;
  nextfl.sec_ref() += freelist_inc;

  while (age_objects.size() < 10) age_objects.push_back( list<object_t>() );
  
  if (fake_size_mb) {
	int fake_bl = fake_size_mb * 256;
	struct statfs st;
	store->statfs(&st);
	float f = (float)fake_bl / (float)st.f_blocks;
	high_water = (float)high_water * f;
	low_water = (float)low_water * f;
	final_water = (float)final_water * f;
	dout(2) << "fake " << fake_bl << " / " << st.f_blocks << " is " << f << ", high " << high_water << " low " << low_water << " final " << final_water << endl;
  }
  
  // init size distn (once)
  if (!did_distn) {
	did_distn = true;
	age_cur_oid = 1;
	file_size_distn.add(1, 19.0758125+0.65434375);
	file_size_distn.add(512, 35.6566);
	file_size_distn.add(1024, 27.7271875);
	file_size_distn.add(2*1024, 16.63503125);
	//file_size_distn.add(4*1024, 106.82384375);
	//file_size_distn.add(8*1024, 81.493375);
	//file_size_distn.add(16*1024, 14.13553125);
	//file_size_distn.add(32*1024, 2.176);
	//file_size_distn.add(256*1024, 0.655938);
	//file_size_distn.add(512*1024, 0.1480625);
	//file_size_distn.add(1*1024*1024, 0.020125); // actually 2, but 32bit
	file_size_distn.normalize();
  }
  
  // clear
  for (int i=0; i<10; i++)
	age_objects[i].clear();
  
  ObjectStore::FragmentationStat st;

  __uint64_t wrote = 0;

  for (int c=1; c<=count; c++) {
	if (g_clock.now() > until) break;
	
	if (c == 7) start_debug = true;
	
	dout(1) << "age " << c << "/" << count << " filling to " << high_water << endl;
	__uint64_t w = age_fill(high_water, until);
	dout(1) << "age wrote " << w << endl;
	wrote += w;
	store->sync();
	//store->_get_frag_stat(st);
	//pfrag(st);



	if (c == count) {
	  dout(1) << "age final empty to " << final_water << endl;
	  age_empty(final_water);	
	} else {
	  dout(1) << "age " << c << "/" << count << " emptying to " << low_water << endl;
	  age_empty(low_water);
	}
	store->sync();
	store->_get_frag_stat(st);
	pfrag(st);

	// dump freelist?
	if (g_clock.now() > nextfl) {
	  elapsed += freelist_inc;
	  save_freelist(elapsed);
	  nextfl.sec_ref() += freelist_inc;
	}
  }

  dout(1) << wrote / (1024ULL*1024ULL) << " GB written" << endl;

  // dump the freelist
  save_freelist(0);
  exit(0);   // hack

  // ok!
  store->_fake_writes(false);
  store->sync();
  store->sync();
  dout(1) << "age finished" << endl;
}  


void Ager::load_freelist()
{
  struct stat st;
  
  int r = ::stat("ebofs.freelist", &st);
  assert(r == 0);

  bufferptr bp = new buffer(st.st_size);
  bufferlist bl;
  bl.push_back(bp);
  int fd = ::open("ebofs.freelist", O_RDONLY);
  ::read(fd, bl.c_str(), st.st_size);
  ::close(fd);

  ((Ebofs*)store)->_import_freelist(bl);
  store->sync();
  store->sync();
}

void Ager::save_freelist(int el)
{
  dout(1) << "save_freelist " << el << endl;
  char s[100];
  sprintf(s, "ebofs.freelist.%d", el);
  bufferlist bl;
  ((Ebofs*)store)->_export_freelist(bl);
  ::unlink(s);
  int fd = ::open(s, O_CREAT|O_WRONLY);
  ::fchmod(fd, 0644);
  ::write(fd, bl.c_str(), bl.length());
  ::close(fd);
}
