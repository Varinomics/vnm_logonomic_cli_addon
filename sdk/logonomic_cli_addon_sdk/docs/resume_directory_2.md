# resume-directory@2

This optional capability states that a package can resume an existing native
conversation in a folder other than the one it was started in. Its empty
declaration is `{"capability_id":"resume-directory","version":2,"payload":{}}`.
It requires `interactive-resume@2` in the same live package generation.

A host resumes a conversation in the folder it is registered with. When that
folder no longer exists, the host may offer to resume the conversation in a
folder the user chooses, and it offers that only for a package that declares
this capability. The chosen folder reaches the package as the ordinary
`context.working_directory` of `prepare_resume` (and of `inspect_resume`); the
request carries no additional member and the result no additional
acknowledgment.

By declaring the capability, the package undertakes that its exact-resume
recipe finds the conversation by its identity rather than by the working
directory, and resumes that same native conversation, account and profile with
the working directory as the conversation's directory. A fork, a new
conversation, a shell `cd`, a transcript hint or a command-specific
working-directory override does not meet that undertaking. A launcher or
configuration that cannot meet it must refuse preparation rather than resume
elsewhere or start a different conversation.

A package whose native store or lookup is keyed by the working directory, or
whose native resume reselects the conversation's own project folder, does not
declare this capability. The host then tells the user that the conversation
cannot be resumed in another folder.
