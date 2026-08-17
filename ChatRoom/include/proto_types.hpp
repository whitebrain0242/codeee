#pragma once

#include "chat_message.pb.h"
#include "file_transfer.pb.h"
#include "friend_event.pb.h"
#include "group_message.pb.h"

using FriendEventPayload = chatroom::v7::FriendEventPayload;
using ChatMessagePayload = chatroom::v7::ChatMessagePayload;
using GroupMessagePayload = chatroom::v8::GroupMessagePayload;
using FileTransferMetadata = chatroom::v9::FileTransferMetadata;
using FileUploadResumeState = chatroom::v9::FileUploadResumeState;