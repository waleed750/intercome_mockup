enum IntercomNotificationAction {
  answer('answer'),
  reject('reject');

  const IntercomNotificationAction(this.id);

  final String id;

  static IntercomNotificationAction? fromId(String? id) {
    for (final action in values) {
      if (action.id == id) return action;
    }
    return null;
  }
}
