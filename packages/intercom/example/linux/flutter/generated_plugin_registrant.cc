//
//  Generated file. Do not edit.
//

// clang-format off

#include "generated_plugin_registrant.h"

#include <intercom/intercom_plugin.h>

void fl_register_plugins(FlPluginRegistry* registry) {
  g_autoptr(FlPluginRegistrar) intercom_registrar =
      fl_plugin_registry_get_registrar_for_plugin(registry, "IntercomPlugin");
  intercom_plugin_register_with_registrar(intercom_registrar);
}
