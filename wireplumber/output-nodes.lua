-- xdg-desktop-portal
--
-- Copyright © 2024 tytan652
--
-- SPDX-License-Identifier: LGPL-2.1-or-later

log = Log.open_topic ("s-xdg-desktop-portal")

-- Object manager to find a WirePlumber session manager
clients_wp = ObjectManager {
  Interest {
    type = "client",
    Constraint { "wireplumber.daemon", "=", true, type = "pw" },
    Constraint { "wireplumber.export-core", "-", type = "pw" },
  }
}

-- Object manager to find portal clients
clients_portal = ObjectManager {
  Interest {
    type = "client",
    Constraint { "pipewire.access", "=", "portal" },
  }
}

-- Object manager to find output nodes
nodes_om = ObjectManager {
  Interest {
    type = "node",
    Constraint { "media.class", "#", "Stream/Output/*" },
  }
}

nodes_om:connect("object-added", function (om, node)
  -- Check if node has a portal client
  for client_portal in clients_portal:iterate {
    Constraint { "object.id", "=", node.properties["client.id"] }
  } do
    for client_wp in clients_wp:iterate () do
      local id = node["bound-id"]
      -- Allow session manager to link provider output node
      -- without requiring the owning client to see the input node
      log:info (client_wp, "Granting \"L\" permission to session manager for node" .. id)
      client_wp:update_permissions { [id] = "rwxml" }
    end
  end
end)

clients_wp:activate ()
clients_portal:activate ()
nodes_om:activate ()