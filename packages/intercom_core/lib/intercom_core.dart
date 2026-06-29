library intercom_core;

// Protocol
export 'src/protocol/alaw_codec.dart';
export 'src/protocol/commands.dart';
export 'src/protocol/discovery.dart';
export 'src/protocol/frame.dart';
export 'src/protocol/frame_parser.dart';

// Call
export 'src/call/call_controller.dart';
export 'src/call/call_phase.dart';
export 'src/call/call_ui_state.dart';
export 'src/call/incoming_call_handler.dart';

// Transport
export 'src/transport/connection_provider.dart';
export 'src/transport/lan_connection_provider.dart';

// Net
export 'src/net/call_connection.dart';
export 'src/net/call_server.dart';
export 'src/net/discovery_responder.dart';
export 'src/net/local_ip_address.dart';

// Media
export 'src/media/audio_pipeline.dart';
export 'src/media/video_decoder.dart';

// Config
export 'src/config/device_config.dart';
export 'src/config/device_identity.dart';

// Background
export 'src/background/android_foreground_service.dart';

// Handlers (optional — use or implement your own IncomingCallHandler)
export 'src/handlers/full_screen_call_handler.dart';
export 'src/handlers/simple_notification_handler.dart';
export 'src/handlers/silent_call_handler.dart';

// UI (optional — use these screens or build your own)
export 'src/ui/screens/idle_screen.dart';
export 'src/ui/screens/incoming_call_screen.dart';
export 'src/ui/screens/in_call_screen.dart';
export 'src/ui/screens/add_intercom_screen.dart';
export 'src/ui/widgets/video_surface.dart';
export 'src/ui/widgets/round_action_button.dart';
export 'src/ui/widgets/pulsing_text.dart';
export 'src/ui/widgets/status_pill.dart';
export 'src/ui/theme/intercom_theme.dart';

// Module
export 'src/intercom_module.dart';
