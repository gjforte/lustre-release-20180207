import sys, types, string, os
import re, exceptions
import xml.dom.minidom
import Lustre

# ============================================================
# XML processing and query

class LustreDB:
    def lookup(self, uuid):
        """ lookup returns a new LustreDB instance"""
        return self._lookup_by_uuid(uuid)

    def lookup_name(self, name, class_name = ""):
        """ lookup returns a new LustreDB instance"""
        return self._lookup_by_name(name, class_name)

    def lookup_class(self, class_name):
        """ lookup returns a new LustreDB instance"""
        return self._lookup_by_class(class_name)

    def get_val(self, tag, default=None):
        v =  self._get_val(tag)
        if v:
            return v
        if default != None:
            return default
        return None

    def get_class(self):
        return self._get_class()

    def get_val_int(self, tag, default=0):
        str = self._get_val(tag)
        try:
            if str:
                return int(str)
            return default
        except ValueError:
            raise Lustre.LconfError("text value is not integer: " + str)
            
    def get_first_ref(self, tag):
        """ Get the first uuidref of the type TAG. Only
        one is expected.  Returns the uuid."""
        uuids = self._get_refs(tag)
        if len(uuids) > 0:
            return  uuids[0]
        return None
    
    def get_refs(self, tag):
        """ Get all the refs of type TAG.  Returns list of uuids. """
        uuids = self._get_refs(tag)
        return uuids

    def get_all_refs(self):
        """ Get all the refs.  Returns list of uuids. """
        uuids = self._get_all_refs()
        return uuids

    def nid2server(self, nid, net_type):
        netlist = self.lookup_class('network')
        for net_db in netlist:
            if net_db.get_val('nid') == nid and net_db.get_val('nettype') == net_type: 
                return net_db
        return None
    
    # Find the target_device for target on a node
    # node->profiles->device_refs->target
    def get_node_tgt_dev(self, node_name, target_uuid):
        node_db = self.lookup_name(node_name)
        if not node_db:
            return None
        return self.get_tgt_dev(target_uuid)

    # get all network uuids for this node
    def get_networks(self):
        ret = []
        prof_list = self.get_refs('profile')
        for prof_uuid in prof_list:
            prof_db = self.lookup(prof_uuid)
            net_list = prof_db.get_refs('network')
            for net_uuid in net_list:
                ret.append(net_uuid)
        return ret

    def get_active_dev(self, tgtuuid):
        tgt = self.lookup(tgtuuid)
        tgt_dev_uuid =tgt.get_first_ref('active')
        return tgt_dev_uuid

    def get_tgt_dev(self, tgtuuid):
        prof_list = self.get_refs('profile')
        for prof_uuid in prof_list:
            prof_db = self.lookup(prof_uuid)
            if not prof_db:
                panic("profile:", profile, "not found.")
            for ref_class, ref_uuid in prof_db.get_all_refs(): 
                if ref_class in ('osd', 'mdsdev'):
                    devdb = self.lookup(ref_uuid)
                    uuid = devdb.get_first_ref('target')
                    if tgtuuid == uuid:
                        return ref_uuid
        return None

    def get_group(self, group):
        ret = []
        devs = self.lookup_class('mds')
        for tgt in devs:
            if tgt.get_val('group', "") == group:
                ret.append(tgt.getUUID())
        devs = self.lookup_class('ost')
        for tgt in devs:
            if tgt.get_val('group', "") == group:
                ret.append(tgt.getUUID())
        return ret

    # Change the current active device for a target
    def update_active(self, tgtuuid, new_uuid):
        self._update_active(tgtuuid, new_uuid)

    def get_version(self):
        return self.get_val('version')

class LustreDB_XML(LustreDB):
    def __init__(self, dom, root_node):
        # init xmlfile
        self.dom_node = dom
        self.root_node = root_node

    def xmltext(self, dom_node, tag):
        list = dom_node.getElementsByTagName(tag)
        if len(list) > 0:
            dom_node = list[0]
            dom_node.normalize()
            if dom_node.firstChild:
                txt = string.strip(dom_node.firstChild.data)
                if txt:
                    return txt

    def xmlattr(self, dom_node, attr):
        return dom_node.getAttribute(attr)

    def _get_val(self, tag):
        """a value could be an attribute of the current node
        or the text value in a child node"""
        ret  = self.xmlattr(self.dom_node, tag)
        if not ret:
            ret = self.xmltext(self.dom_node, tag)
        return ret

    def _get_class(self):
        return self.dom_node.nodeName

    def get_ref_type(self, ref_tag):
        res = string.split(ref_tag, '_')
        return res[0]

    #
    # [(ref_class, ref_uuid),]
    def _get_all_refs(self):
        list = []
        for n in self.dom_node.childNodes: 
            if n.nodeType == n.ELEMENT_NODE:
                ref_uuid = self.xml_get_ref(n)
                ref_class = self.get_ref_type(n.nodeName)
                list.append((ref_class, ref_uuid))
                    
        list.sort()
        return list

    def _get_refs(self, tag):
        """ Get all the refs of type TAG.  Returns list of uuids. """
        uuids = []
        refname = '%s_ref' % tag
        reflist = self.dom_node.getElementsByTagName(refname)
        for r in reflist:
            uuids.append(self.xml_get_ref(r))
        return uuids

    def xmllookup_by_uuid(self, dom_node, uuid):
        for n in dom_node.childNodes:
            if n.nodeType == n.ELEMENT_NODE:
                if self.xml_get_uuid(n) == uuid:
                    return n
                else:
                    n = self.xmllookup_by_uuid(n, uuid)
                    if n: return n
        return None

    def _lookup_by_uuid(self, uuid):
        dom = self. xmllookup_by_uuid(self.root_node, uuid)
        if dom:
            return LustreDB_XML(dom, self.root_node)

    def xmllookup_by_name(self, dom_node, name):
        for n in dom_node.childNodes:
            if n.nodeType == n.ELEMENT_NODE:
                if self.xml_get_name(n) == name:
                    return n
                else:
                    n = self.xmllookup_by_name(n, name)
                    if n: return n
        return None

    def _lookup_by_name(self, name, class_name):
        dom = self.xmllookup_by_name(self.root_node, name)
        if dom:
            return LustreDB_XML(dom, self.root_node)

    def xmllookup_by_class(self, dom_node, class_name):
        return dom_node.getElementsByTagName(class_name)

    def _lookup_by_class(self, class_name):
        ret = []
        domlist = self.xmllookup_by_class(self.root_node, class_name)
        for node in domlist:
            ret.append(LustreDB_XML(node, self.root_node))
        return ret

    def xml_get_name(self, n):
        return n.getAttribute('name')
        
    def getName(self):
        return self.xml_get_name(self.dom_node)

    def xml_get_ref(self, n):
        return n.getAttribute('uuidref')

    def xml_get_uuid(self, dom_node):
        return dom_node.getAttribute('uuid')

    def getUUID(self):
        return self.xml_get_uuid(self.dom_node)

    # Convert routes from the router to a route that will be used
    # on the local system.  The network type and gw are changed to the
    # interface on the router the local system will connect to.
    def get_local_routes(self, type, gw):
        """ Return the routes as a list of tuples of the form:
        [(type, gw, lo, hi),]"""
        res = []
        tbl = self.dom_node.getElementsByTagName('routetbl')
        for t in tbl:
            routes = t.getElementsByTagName('route')
            for r in routes:
                net_type = self.xmlattr(r, 'type')
                if type != net_type:
                    lo = self.xmlattr(r, 'lo')
                    hi = self.xmlattr(r, 'hi')
                    tgt_cluster_id = self.xmlattr(r, 'tgtclusterid')
                    res.append((type, gw, tgt_cluster_id, lo, hi))
        return res

    def get_route_tbl(self):
        ret = []
        for r in self.dom_node.getElementsByTagName('route'):
            net_type = self.xmlattr(r, 'type')
            gw = self.xmlattr(r, 'gw')
            gw_cluster_id = self.xmlattr(r, 'gwclusterid')
            tgt_cluster_id = self.xmlattr(r, 'tgtclusterid')
            lo = self.xmlattr(r, 'lo')
            hi = self.xmlattr(r, 'hi')
            ret.append((net_type, gw, gw_cluster_id, tgt_cluster_id, lo, hi))
        return ret

    def _update_active(self, tgt, new):
        raise Lustre.LconfError("updates not implemented for XML")

# ================================================================    
# LDAP Support
class LustreDB_LDAP(LustreDB):
    def __init__(self, name, attrs,
                 base = "fs=lustre",
                 parent = None,
                 url  = "ldap://localhost",
                 user = "cn=Manager, fs=lustre",
                 pw   = "secret"
                 ):
        self._name = name
        self._attrs = attrs
        self._base = base
        self._parent = parent
        self._url  = url
        self._user = user
        self._pw   = pw
        if parent:
            self.l = parent.l
            self._base = parent._base
        else:
            self.open()

    def open(self):
        import ldap
        try:
            self.l = ldap.initialize(self._url)
            # Set LDAP protocol version used
            self.l.protocol_version=ldap.VERSION3
            # user and pw only needed if modifying db
            self.l.bind_s(self._user, self._pw, ldap.AUTH_SIMPLE);
        except ldap.LDAPError, e:
            raise Lustre.LconfError('Unable to connection to ldap server')

        try:
            self._name, self._attrs = self.l.search_s(self._base,
                                                      ldap.SCOPE_BASE)[0]
        except ldap.LDAPError, e:
            raise Lustre.LconfError("no config found in ldap: %s"
                                      % (self._base,))
    def close(self):
        self.l.unbind_s()

    def ldap_search(self, filter):
        """Return list of uuids matching the filter."""
        import ldap
        dn = self._base
        ret = []
        uuids = []
        try:
            for name, attrs in self.l.search_s(dn, ldap.SCOPE_ONELEVEL,
                                        filter, ["uuid"]):
                for v in attrs['uuid']:
                    uuids.append(v)
        except ldap.NO_SUCH_OBJECT, e:
            pass
        except ldap.LDAPError, e:
            print e                     # FIXME: die here?
        if len(uuids) > 0:
            for uuid in uuids:
                ret.append(self._lookup_by_uuid(uuid))
        return ret

    def _lookup_by_name(self, name, class_name):
        list =  self.ldap_search("lustreName=%s" %(name))
        if len(list) == 1:
            return list[0]
        return None

    def _lookup_by_class(self, class_name):
        return self.ldap_search("objectclass=%s" %(string.upper(class_name)))

    def _lookup_by_uuid(self, uuid):
        import ldap
        dn = "uuid=%s,%s" % (uuid, self._base)
        ret = None
        try:
            for name, attrs in self.l.search_s(dn, ldap.SCOPE_BASE,
                                               "objectclass=*"):
                ret = LustreDB_LDAP(name, attrs,  parent = self)
                        
        except ldap.NO_SUCH_OBJECT, e:
            pass                        # just return empty list
        except ldap.LDAPError, e:
            print e                     # FIXME: die here?
        return ret


    def _get_val(self, k):
        ret = None
        if self._attrs.has_key(k):
            v = self._attrs[k]
            if type(v) == types.ListType:
                ret = str(v[0])
            else:
                ret = str(v)
        return ret

    def _get_class(self):
        return string.lower(self._attrs['objectClass'][0])

    def get_ref_type(self, ref_tag):
        return ref_tag[:-3]

    #
    # [(ref_class, ref_uuid),]
    def _get_all_refs(self):
        list = []
        for k in self._attrs.keys():
            if re.search('.*Ref', k):
                for uuid in self._attrs[k]:
                    ref_class = self.get_ref_type(k)
                    list.append((ref_class, uuid))
        return list

    def _get_refs(self, tag):
        """ Get all the refs of type TAG.  Returns list of uuids. """
        uuids = []
        refname = '%sRef' % tag
        if self._attrs.has_key(refname):
            return self._attrs[refname]
        return []

    def getName(self):
        return self._get_val('lustreName')

    def getUUID(self):
        return self._get_val('uuid')

    def get_route_tbl(self):
        return []

    def _update_active(self, tgtuuid, newuuid):
        """Return list of uuids matching the filter."""
        import ldap
        dn = "uuid=%s,%s" %(tgtuuid, self._base)
        ret = []
        uuids = []
        try:
            self.l.modify_s(dn, [(ldap.MOD_REPLACE, "activeRef", newuuid)])
        except ldap.NO_SUCH_OBJECT, e:
            print e
        except ldap.LDAPError, e:
            print e                     # FIXME: die here?
        return 
