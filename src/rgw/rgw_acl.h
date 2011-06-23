#ifndef CEPH_RGW_ACL_H
#define CEPH_RGW_ACL_H

#include <map>
#include <string>
#include <iostream>
#include <include/types.h>

#include <expat.h>

#include "rgw_xml.h"

using namespace std;


#define RGW_URI_ALL_USERS	"http://acs.amazonaws.com/groups/global/AllUsers"
#define RGW_URI_AUTH_USERS	"http://acs.amazonaws.com/groups/global/AuthenticatedUsers"

#define RGW_PERM_READ            0x01
#define RGW_PERM_WRITE           0x02
#define RGW_PERM_READ_ACP        0x04
#define RGW_PERM_WRITE_ACP       0x08
#define RGW_PERM_FULL_CONTROL    ( RGW_PERM_READ | RGW_PERM_WRITE | \
                                  RGW_PERM_READ_ACP | RGW_PERM_WRITE_ACP )
#define RGW_PERM_ALL             RGW_PERM_FULL_CONTROL

class ACLPermission : public XMLObj
{
  int flags;
public:
  ACLPermission();
  ~ACLPermission();
  bool xml_end(const char *el);
  int get_permissions();
  void set_permissions(int perm);

  void encode(bufferlist& bl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(flags, bl);
  }
  void decode(bufferlist::iterator& bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(flags, bl);
  }
  void to_xml(ostream& out);
};
WRITE_CLASS_ENCODER(ACLPermission)

enum ACLGranteeTypeEnum {
  ACL_TYPE_CANON_USER,
  ACL_TYPE_EMAIL_USER,
  ACL_TYPE_GROUP,
  ACL_TYPE_UNKNOWN,
};
class ACLGranteeType
{
  __u32 type;
public:
  ACLGranteeType();
  ~ACLGranteeType();
  const char *to_string();
  ACLGranteeTypeEnum get_type();
  void set(ACLGranteeTypeEnum t);
  void set(const char *s);
  void encode(bufferlist& bl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(type, bl);
  }
  void decode(bufferlist::iterator& bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(type, bl);
  }
};
WRITE_CLASS_ENCODER(ACLGranteeType)

class ACLGrantee : public XMLObj
{
  string type;
public:
  ACLGrantee();
  ~ACLGrantee();

  bool xml_start(const char *el, const char **attr);

  string& get_type();
};


class ACLGrant : public XMLObj
{
  ACLGranteeType type;

  string id;
  string uri;
  string email;
  ACLPermission permission;
  string name;
public:
  ACLGrant();
  ~ACLGrant();

  bool xml_end(const char *el);

  /* there's an assumption here that email/uri/id encodings are
     different and there can't be any overlap */
  string& get_id() {
    switch(type.get_type()) {
    case ACL_TYPE_EMAIL_USER:
      return email;
    case ACL_TYPE_GROUP:
      return uri;
    default:
      return id;
    }
  }
  ACLGranteeType& get_type() { return type; }
  ACLPermission& get_permission() { return permission; }

  void encode(bufferlist& bl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(type, bl);
    ::encode(id, bl);
    ::encode(uri, bl);
    ::encode(email, bl);
    ::encode(permission, bl);
    ::encode(name, bl);
  }
  void decode(bufferlist::iterator& bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(type, bl);
    ::decode(id, bl);
    ::decode(uri, bl);
    ::decode(email, bl);
    ::decode(permission, bl);
    ::decode(name, bl);
  }
  void to_xml(ostream& out) {
    out << "<Grant>" <<
            "<Grantee xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"" << type.to_string() << "\">";
    switch (type.get_type()) {
    case ACL_TYPE_CANON_USER:
      out << "<ID>" << id << "</ID>" <<
             "<DisplayName>" << name << "</DisplayName>";
      break;
    case ACL_TYPE_EMAIL_USER:
      out << "<EmailAddress>" << email << "</EmailAddress>";
      break;
    case ACL_TYPE_GROUP:
       out << "<URI>" << uri << "</URI>";
      break;
    default:
      break;
    }
    out << "</Grantee>";
    permission.to_xml(out);
    out << "</Grant>";
  }
  void set_canon(string& _id, string& _name, int perm) {
    type.set(ACL_TYPE_CANON_USER);
    id = _id;
    name = _name;
    permission.set_permissions(perm);
  }
  void set_group(string& _uri, int perm) {
    type.set(ACL_TYPE_GROUP);
    uri = _uri;
    permission.set_permissions(perm);
  }
  bool xml_start(const char *el, const char **attr);
};
WRITE_CLASS_ENCODER(ACLGrant)

class RGWAccessControlList : public XMLObj
{
  map<string, int> acl_user_map;
  multimap<string, ACLGrant> grant_map;
  bool user_map_initialized;

  void init_user_map();
public:
  RGWAccessControlList();
  ~RGWAccessControlList();

  bool xml_end(const char *el);
  int get_perm(CephContext *cct, string& id, int perm_mask);
  void encode(bufferlist& bl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(user_map_initialized, bl);
    ::encode(acl_user_map, bl);
    ::encode(grant_map, bl);
  }
  void decode(bufferlist::iterator& bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(user_map_initialized, bl);
    ::decode(acl_user_map, bl);
    ::decode(grant_map, bl);
  }
  void to_xml(ostream& out) {
    map<string, ACLGrant>::iterator iter;
    out << "<AccessControlList>";
    for (iter = grant_map.begin(); iter != grant_map.end(); ++iter) {
      ACLGrant& grant = iter->second;
      grant.to_xml(out);
    }
    out << "</AccessControlList>";
  }
  void add_grant(ACLGrant *grant);

  void create_default(string id, string name) {
    acl_user_map.clear();
    grant_map.clear();

    ACLGrant grant;
    grant.set_canon(id, name, RGW_PERM_FULL_CONTROL);
    add_grant(&grant);
  }
  bool create_canned(string id, string name, string canned_acl);
};
WRITE_CLASS_ENCODER(RGWAccessControlList)

class ACLOwner : public XMLObj
{
  string id;
  string display_name;
public:
  ACLOwner();
  ~ACLOwner();

  bool xml_end(const char *el);
  void encode(bufferlist& bl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(id, bl);
    ::encode(display_name, bl);
  }
  void decode(bufferlist::iterator& bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(id, bl);
    ::decode(display_name, bl);
  }
  void to_xml(ostream& out) {
    if (id.empty())
      return;
    out << "<Owner>" << "<ID>" << id << "</ID>";
    if (!display_name.empty())
      out << "<DisplayName>" << display_name << "</DisplayName>";
    out << "</Owner>";
  }
  void set_id(string& _id) { id = _id; }
  void set_name(string& name) { display_name = name; }

  string& get_id() { return id; }
  string& get_display_name() { return display_name; }
};
WRITE_CLASS_ENCODER(ACLOwner)

class RGWAccessControlPolicy : public XMLObj
{
  RGWAccessControlList acl;
  ACLOwner owner;

public:
  RGWAccessControlPolicy();
  ~RGWAccessControlPolicy();

  bool xml_end(const char *el);

  int get_perm(CephContext *cct, string& id, int perm_mask);

  void encode(bufferlist& bl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(owner, bl);
    ::encode(acl, bl);
  }
  void decode(bufferlist::iterator& bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(owner, bl);
    ::decode(acl, bl);
   }
  void decode_owner(bufferlist::iterator& bl) { // sometimes we only need that, should be faster
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(owner, bl);
  }
  void to_xml(ostream& out) {
    out << "<AccessControlPolicy xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">";
    owner.to_xml(out);
    acl.to_xml(out);
    out << "</AccessControlPolicy>";
  }

  void set_owner(ACLOwner& o) { owner = o; }
  ACLOwner& get_owner() {
    return owner;
  }

  void create_default(string& id, string& name) {
    acl.create_default(id, name);
    owner.set_id(id);
    owner.set_name(name);
  }
  bool create_canned(string id, string name, string canned_acl) {
    bool ret = acl.create_canned(id, name, canned_acl);
    owner.set_id(id);
    owner.set_name(name);
    return ret;
  }

  RGWAccessControlList& get_acl() {
    return acl;
  }
};
WRITE_CLASS_ENCODER(RGWAccessControlPolicy)

/**
 * Interfaces with the webserver's XML handling code
 * to parse it in a way that makes sense for the rgw.
 */
class RGWACLXMLParser : public RGWXMLParser
{
  XMLObj *alloc_obj(const char *el);
public:
  RGWACLXMLParser() {}
};

#endif
